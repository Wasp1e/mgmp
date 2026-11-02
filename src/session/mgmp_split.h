#pragma once
// mgmp_split -- who controls which cats, as a table that lasts the whole run.
//
// Extracted for the same reason td_decide and barrier_decide are: exercising it
// in the game needs three or four live instances and a battle with the right
// number of human cats, which is an expensive way to find out that a division
// rounded the wrong way. Here it is a table.
//
// WHY A TABLE AND NOT ARITHMETIC OVER THE ROSTER. The roster is not a stable
// order: the game rebuilds the character list each battle and a cat whose speed
// changed moves within it. Dividing that list by position handed cats from one
// player to the other mid-run. Measured 2026-10-31: the 36 hp cat was the
// friend's for five battles and the host's in the sixth.
//
// THE RULE. A cat is identified by its persistent id, CatData+0x00, and keeps
// the player it was first given to until the run ends. A cat not seen yet goes
// to whoever has the fewest cats in this battle, earliest position on a tie, and
// unseen cats are taken in roster order: the roster is by speed, so the players
// alternate through the first battle. Taken in id order they did not, measured
// 2026-11-02: host, friend, friend, host. Speed gained later can still put two
// of one player's cats side by side, because no cat changes hands to fix it.
//
//   4 cats, 2 players  ->  2 / 2
//   3 cats, 2 players  ->  2 / 1        (the host takes the odd cat, as ever)
//   4 cats, 3 players  ->  2 / 1 / 1
//   2 cats, 4 players  ->  1 / 1 / 0 / 0
//
// A CAT THAT DIES TAKES NOBODY WITH IT. Arithmetic over the survivors would move
// a cat across the split; the table does not. The cost is that the shares can
// end up uneven, which is the point: nobody's cat changes hands.
//
// POSITION, NOT PEER ID. `pos` is an index into the sorted membership list. The
// two are the same until somebody disconnects, after which ids have a gap and
// positions do not. A position past the end of the list has no player behind
// it, so that cat is covered for this battle and its row is left alone: when the
// player returns the cat is theirs again.
//
// THE HOST'S TABLE IS THE TABLE. It travels in ENTERNODE, so a client that
// restarted mid-run is not left working it out from battles it never saw. The
// result is still cross-checked on the wire: every player publishes what it
// claims in CONTROL, and the tally must show each human cat claimed by exactly
// one player.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mgmp {

constexpr uint32_t kMaxOwners = 16;      // ENTERNODE's limit; a party is 4
constexpr uint32_t kMaxSplitPeers = 32;

struct OwnerTable {
    uint64_t id[kMaxOwners]  = {};
    uint8_t  pos[kMaxOwners] = {};
    uint32_t count           = 0;

    int find(uint64_t cat) const {
        for (uint32_t i = 0; i < count; ++i)
            if (id[i] == cat) return (int)i;
        return -1;
    }
};

// `ids` holds the battle's human cats in roster order, 0 for a cat with no
// CatData; such a cat cannot be remembered and is placed afresh each battle.
// `peers` of 0 is treated as 1. With `remember` false nothing is written to the
// table, for a session that does not have its players yet. out[i] receives the
// position of the player who owns ids[i].
inline void split_assign(OwnerTable& t, const uint64_t* ids, uint32_t humans,
                         uint32_t peers, bool remember, uint8_t* out) {
    if (peers == 0) peers = 1;
    if (peers > kMaxSplitPeers) peers = kMaxSplitPeers;

    constexpr uint8_t kUnplaced = 0xFF;
    uint32_t load[kMaxSplitPeers] = {};

    for (uint32_t i = 0; i < humans; ++i) {
        out[i] = kUnplaced;
        const int row = ids[i] ? t.find(ids[i]) : -1;
        if (row >= 0 && t.pos[row] < peers) {
            out[i] = t.pos[row];
            ++load[out[i]];
        }
    }

    for (uint32_t i = 0; i < humans; ++i) {
        if (out[i] != kUnplaced) continue;

        uint32_t best = 0;
        for (uint32_t p = 1; p < peers; ++p)
            if (load[p] < load[best]) best = p;

        out[i] = (uint8_t)best;
        ++load[best];

        if (remember && ids[i] && t.find(ids[i]) < 0 && t.count < kMaxOwners) {
            t.id[t.count]  = ids[i];
            t.pos[t.count] = (uint8_t)best;
            ++t.count;
        }
    }
}

// THE TABLE ON DISK. Reloading a run starts the table over, and the roster it is
// rebuilt from has moved with every speed gained, so the same run came back with
// two cats swapped: measured 2026-11-03. The host keeps a file, one "id position"
// line per cat. A cat's id outlives a run, so the file carries no run id: it
// applies when it names every cat in the run.
inline uint32_t split_format(const OwnerTable& t, char* out, uint32_t cap) {
    uint32_t n = 0;
    if (cap) out[0] = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const int w = snprintf(out + n, cap - n, "%016llx %u\n",
                               (unsigned long long)t.id[i], (unsigned)t.pos[i]);
        if (w < 0 || (uint32_t)w >= cap - n) break;
        n += (uint32_t)w;
    }
    return n;
}

inline OwnerTable split_parse(const char* text) {
    OwnerTable t;
    while (*text && t.count < kMaxOwners) {
        unsigned long long id = 0;
        unsigned pos = 0;
        if (sscanf_s(text, "%llx %u", &id, &pos) == 2 && id && pos < 256 &&
            t.find(id) < 0) {
            t.id[t.count]  = id;
            t.pos[t.count] = (uint8_t)pos;
            ++t.count;
        }
        const char* eol = strchr(text, '\n');
        if (!eol) break;
        text = eol + 1;
    }
    return t;
}

inline bool split_covers(const OwnerTable& t, const uint64_t* ids, uint32_t n) {
    if (!n) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (t.find(ids[i]) < 0) return false;
    return true;
}

} // namespace mgmp
