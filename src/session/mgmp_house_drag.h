#pragma once
#include <cstdint>
#include <cmath>

namespace mgmp {

// Tracks remote drag leases. No game pointers or game writes.
// A per-sender sequence rejects older samples; an expired lease releases a
// cat after disconnect or packet loss. Call reset() when leaving the session.
class HouseDragTracker {
public:
    static constexpr uint8_t kNoPeer = 255;
    static constexpr uint64_t kLeaseMs = 1500;
    struct Slot {
        uint64_t key = 0, sequence = 0, last_at = 0;
        float x = 0, y = 0;
        uint64_t placed_sequence = 0;
    };

    void reset() { for (auto& s : slots_) s = Slot{}; }

    void expire(uint64_t now) {
        for (auto& s : slots_)
            if (s.key && now >= s.last_at && now - s.last_at > kLeaseMs)
                s.key = 0;
    }

    void exclude_local(uint64_t key, uint8_t self = 0) {
        if (!key) return;
        for (unsigned i = self; i < kNoPeer; ++i)
            if (slots_[i].key == key) slots_[i].key = 0;
    }

    bool sample(uint8_t peer, uint64_t key, uint64_t sequence, float x, float y,
                uint64_t now, uint64_t local_key, uint8_t self = 0) {
        if (peer == kNoPeer || peer == self || !key || !sequence ||
            !std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x) > 10000 || std::abs(y) > 10000) return false;
        expire(now);
        Slot& s = slots_[peer];
        if (sequence <= s.sequence) return false;
        s.sequence = sequence;
        // Lowest peer id wins on every screen, independent of arrival order.
        if (key == local_key && self < peer) return false;
        for (unsigned i = 0; i < peer; ++i)
            if (slots_[i].key == key) return false;
        for (unsigned i = peer + 1; i < kNoPeer; ++i)
            if (slots_[i].key == key) slots_[i].key = 0;
        s.key = key; s.last_at = now; s.x = x; s.y = y;
        return true;
    }

    bool release(uint8_t peer, uint64_t key, uint64_t sequence) {
        if (peer == kNoPeer || !key || !sequence) return false;
        Slot& s = slots_[peer];
        if (sequence <= s.sequence) return false;
        s.sequence = sequence;
        if (s.key != key) return false;
        s.key = 0;
        return true;
    }

    bool can_place(uint8_t peer, uint64_t key, uint64_t sequence) const {
        const Slot& s = slots_[peer];
        return peer != kNoPeer && key && sequence > s.placed_sequence &&
               (s.key != key || s.sequence <= sequence);
    }

    void complete_placement(uint8_t peer, uint64_t key, uint64_t sequence) {
        if (!can_place(peer, key, sequence)) return;
        Slot& s = slots_[peer];
        s.placed_sequence = sequence;
        if (s.sequence < sequence) s.sequence = sequence;
        if (s.key == key) s.key = 0;
    }

    const Slot& slot(uint8_t peer) const { return slots_[peer]; }
    uint8_t owner(uint64_t key) const {
        if (key) for (unsigned i = 0; i < kNoPeer; ++i)
            if (slots_[i].key == key) return (uint8_t)i;
        return kNoPeer;
    }

private:
    Slot slots_[256]{};
};

} // namespace mgmp
