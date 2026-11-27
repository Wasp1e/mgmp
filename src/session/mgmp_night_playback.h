#pragma once
#include <cstdint>

namespace mgmp {
// A delayed client must still display each event and its outcome. Commands
// identify the night as well as the scene; scene numbers alone restart at 1.
struct NightPlaybackGate {
    uint64_t epoch = 0, scene_at = 0, outcome_at = 0;
    bool outcome_seen = false, closed = false;
    void reset(uint64_t id) { *this = {}; epoch = id; }
    bool accepts(uint64_t id) const { return epoch && epoch == id; }
    void begin_scene(uint64_t now) {
        scene_at = now; outcome_at = 0; outcome_seen = closed = false;
    }
    void observe_outcome(uint64_t now) {
        if (!outcome_seen) { outcome_at = now; outcome_seen = true; }
    }
    bool can_show(uint64_t now) const { return !closed && now - scene_at >= 350; }
    bool can_close(uint64_t now) const {
        return !closed && outcome_seen && now - scene_at >= 1500 && now - outcome_at >= 750;
    }
};
}
