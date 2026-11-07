// Pins mgmp_split.h -- who controls which cats. In-game this needs live instances
// AND a run in which a cat changes speed or dies, so the rule is pinned here and
// the live run is left to prove only what needs a game.
//
// The property that actually matters is that a cat, once given to a player,
// stays with that player for the rest of the run, whatever the roster does. The
// exhaustive sweep at the bottom covers the other one: nobody carries two more
// cats than anybody else when the cats are all new.

#include "mgmp_split.h"

#include <cstdio>
#include <cstring>

using namespace mgmp;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) { printf("FAIL: %s\n", what); ++failures; }
}

int main() {
    // --- a player who joins after the split --------------------------------
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t ids[4] = { 1, 2, 3, 4 };
        split_assign(t, ids, 4, 2, true, o);           // two players: 0 1 0 1
        split_assign(t, ids, 4, 3, true, o);           // a third joins
        int load[3] = {};
        for (int i = 0; i < 4; ++i) ++load[o[i]];
        check(load[0] == 2 && load[1] == 1 && load[2] == 1,
              "a third player who joins a remembered split gets a cat");
        uint8_t again[4];
        split_assign(t, ids, 4, 3, true, again);
        check(memcmp(o, again, 4) == 0, "and keeps it on the next look");
    }
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t ids[4] = { 1, 2, 3, 4 };
        split_assign(t, ids, 4, 2, true, o);           // 0 1 0 1
        const uint64_t left[2] = { 1, 3 };             // both of player 1's cats gone
        uint8_t l[2];
        split_assign(t, left, 2, 2, true, l);
        check(l[0] == 0 && l[1] == 0, "a player whose cats died does not take the host's");
    }

    // --- shares ------------------------------------------------------------
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t ids[4] = { 40, 10, 30, 20 };
        split_assign(t, ids, 4, 2, true, o);
        check(o[0] == 0 && o[1] == 1 && o[2] == 0 && o[3] == 1,
              "four new cats alternate in roster order, whatever their ids");
    }
    {
        OwnerTable t; uint8_t o[3];
        const uint64_t ids[3] = { 1, 2, 3 };
        split_assign(t, ids, 3, 2, true, o);
        check(o[0] == 0 && o[1] == 1 && o[2] == 0, "the host takes the odd cat");
    }
    {
        OwnerTable t; uint8_t o[2];
        const uint64_t ids[2] = { 1, 2 };
        split_assign(t, ids, 2, 4, true, o);
        check(o[0] == 0 && o[1] == 1, "fewer cats than players: late positions watch");
    }

    // --- THE REPORT: a reordered roster moves nobody -------------------------
    {
        OwnerTable t; uint8_t first[4], later[4];
        const uint64_t before[4] = { 10, 20, 30, 40 };
        const uint64_t after[4]  = { 30, 10, 40, 20 };   // the fast cat moved up
        split_assign(t, before, 4, 2, true, first);
        split_assign(t, after,  4, 2, true, later);
        check(later[0] == first[2] && later[1] == first[0] &&
              later[2] == first[3] && later[3] == first[1],
              "every cat keeps its player when the roster reorders");
    }

    // --- a death moves nobody ------------------------------------------------
    {
        OwnerTable t; uint8_t first[4], later[3];
        const uint64_t all[4]  = { 10, 20, 30, 40 };
        const uint64_t rest[3] = { 20, 30, 40 };
        split_assign(t, all,  4, 2, true, first);
        split_assign(t, rest, 3, 2, true, later);
        check(later[0] == first[1] && later[1] == first[2] && later[2] == first[3],
              "the survivors keep their players after a death");
    }

    // --- a newcomer balances -------------------------------------------------
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t all[3]  = { 10, 20, 30 };    // 2 / 1
        split_assign(t, all, 3, 2, true, o);
        const uint64_t more[4] = { 50, 10, 20, 30 };
        split_assign(t, more, 4, 2, true, o);
        check(o[0] == 1, "a new cat goes to the player with fewer");
        check(t.count == 4, "and is remembered");
    }

    // --- a player who left and came back -------------------------------------
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t ids[4] = { 10, 20, 30, 40 };
        split_assign(t, ids, 4, 2, true, o);
        split_assign(t, ids, 4, 1, true, o);
        check(o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0,
              "a lone player covers every cat");
        split_assign(t, ids, 4, 2, true, o);
        check(o[1] == 1 && o[3] == 1, "the cats go back when the player returns");
    }

    // --- not remembering, and cats with no id --------------------------------
    {
        OwnerTable t; uint8_t o[2];
        const uint64_t ids[2] = { 10, 20 };
        split_assign(t, ids, 2, 2, false, o);
        check(t.count == 0, "remember = false leaves the table alone");
    }
    {
        OwnerTable t; uint8_t o[2];
        const uint64_t ids[2] = { 0, 0 };
        split_assign(t, ids, 2, 2, true, o);
        check(o[0] == 0 && o[1] == 1, "cats without an id go out in roster order");
        check(t.count == 0, "and are not remembered");
    }
    {
        OwnerTable t; uint8_t o[1];
        const uint64_t ids[1] = { 10 };
        split_assign(t, ids, 1, 0, true, o);
        check(o[0] == 0, "peers = 0 is treated as 1");
        split_assign(t, ids, 0, 2, true, o);
        check(t.count == 1, "no human cats changes nothing");
    }
    {
        OwnerTable t; uint8_t o[kMaxOwners + 2];
        uint64_t ids[kMaxOwners + 2];
        for (uint32_t i = 0; i < kMaxOwners + 2; ++i) ids[i] = i + 1;
        split_assign(t, ids, kMaxOwners + 2, 2, true, o);
        check(t.count == kMaxOwners, "a full table stops growing");
        check(o[kMaxOwners + 1] < 2, "and the overflow is still placed");
    }

    // --- the table on disk ---------------------------------------------------
    {
        OwnerTable t; uint8_t o[4];
        const uint64_t ids[4] = { 0x8db411c683615c38ULL, 0x069228a12583872bULL,
                                  0xbe594044d93e9a81ULL, 0xfb397ba372390ef9ULL };
        split_assign(t, ids, 4, 2, true, o);
        char text[256];
        split_format(t, text, sizeof(text));
        const OwnerTable back = split_parse(text);
        check(back.count == 4, "every row survives the file");
        for (uint32_t i = 0; i < 4; ++i)
            check(back.pos[back.find(ids[i])] == o[i], "and keeps its player");
        check(split_covers(back, ids, 4), "the same party is covered");
        const uint64_t dead[3] = { ids[3], ids[0], ids[2] };
        check(split_covers(back, dead, 3), "so is that party after a death");
        const uint64_t other[4] = { ids[0], ids[1], ids[2], 77 };
        check(!split_covers(back, other, 4), "one new cat means another run");
        check(!split_covers(back, ids, 0), "an unread party means nothing");

        // A reload: the fast cat moved up and the table came off the disk.
        OwnerTable loaded = split_parse(text);
        const uint64_t moved[4] = { ids[1], ids[0], ids[2], ids[3] };
        uint8_t later[4];
        split_assign(loaded, moved, 4, 2, true, later);
        check(later[0] == o[1] && later[1] == o[0], "a reload swaps nobody");
    }
    {
        check(split_parse("").count == 0, "an empty file is an empty table");
        check(split_parse("nonsense\n\n12 1").count == 1, "a bad line costs only itself");
        char tiny[8];
        OwnerTable t; t.id[0] = 1; t.count = 1;
        check(split_format(t, tiny, sizeof(tiny)) == 0, "a short buffer writes no half row");
    }

    // --- THE SHARES: balanced and earliest-first for every plausible battle ---
    for (uint32_t peers = 1; peers <= 8; ++peers) {
        for (uint32_t humans = 0; humans <= 24; ++humans) {
            OwnerTable t;
            uint64_t ids[32];
            uint8_t  o[32];
            for (uint32_t i = 0; i < humans; ++i) ids[i] = 1000 - i;
            split_assign(t, ids, humans, peers, true, o);

            uint32_t share[8] = {};
            for (uint32_t i = 0; i < humans; ++i) {
                if (o[i] >= peers) {
                    printf("FAIL: humans=%u peers=%u -- cat %u has no player\n",
                           humans, peers, i);
                    ++failures;
                } else {
                    ++share[o[i]];
                }
            }
            for (uint32_t pos = 0; pos < peers; ++pos) {
                const uint32_t want = humans / peers + (pos < humans % peers ? 1u : 0u);
                if (share[pos] == want) continue;
                printf("FAIL: humans=%u peers=%u pos=%u -- share %u, expected %u\n",
                       humans, peers, pos, share[pos], want);
                ++failures;
            }
        }
    }

    if (failures) { printf("test_split: FAILED -- %d failure(s)\n", failures); return 1; }
    printf("test_split: PASSED -- 0 failure(s)\n");
    return 0;
}
