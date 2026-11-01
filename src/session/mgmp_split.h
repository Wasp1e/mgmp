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
// unseen cats are taken in id order so that speed has no say in it.
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

namespace mgmp {

constexpr uint32_t kMaxOwners = 16;       // ENTERNODE's limit; a party is 4
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

    for (;;) {
        // Lowest id first; equal ids can only be 0, and those stay in roster
        // order, which every peer shares.
        uint32_t next = humans;
        for (uint32_t i = 0; i < humans; ++i)
            if (out[i] == kUnplaced && (next == humans || ids[i] < ids[next])) next = i;
        if (next == humans) return;

        uint32_t best = 0;
        for (uint32_t p = 1; p < peers; ++p)
            if (load[p] < load[best]) best = p;

        out[next] = (uint8_t)best;
        ++load[best];

        if (remember && ids[next] && t.find(ids[next]) < 0 && t.count < kMaxOwners) {
            t.id[t.count]  = ids[next];
            t.pos[t.count] = (uint8_t)best;
            ++t.count;
        }
    }
}

} // namespace mgmp
