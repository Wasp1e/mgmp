#pragma once
#include "mgmp_net.h"
#include "mgmp_log.h"
#include <cstring>

namespace mgmp {
inline bool house_votes[2][kMaxPeers]{};
inline uint64_t house_vote_day = 0;
inline bool house_vote_active = false;
inline bool house_vote_running = false;
inline void house_vote_press(unsigned action) {
    if (!house_vote_active || action > 1 || net_self() >= kMaxPeers) return;
    ChoiceMsg m{};
    m.kind = 57;
    m.node_seed = house_vote_day;
    m.index = action;
    m.aux = !house_votes[action][net_self()];
    if (net_send_choice(m)) {
        house_votes[action][net_self()] = m.aux != 0;
        log_line("HOUSE", "local readiness: action=%s peer=%u ready=%u day=%llu",
                 action ? "set off" : "end day", net_self(), m.aux,
                 (unsigned long long)house_vote_day);
    }
}
inline unsigned house_vote_count(unsigned action) {
    uint8_t peers[kMaxPeers];
    memset(peers, kNoPeer, sizeof(peers));
    if (!net_peer_ids(peers, kMaxPeers)) return 0;
    unsigned count = 0;
    for (uint8_t peer : peers) if (peer < kMaxPeers) count += house_votes[action][peer];
    return count;
}
inline bool house_vote_complete(unsigned action) {
    return house_vote_active && net_role() == NetRole::Host &&
           house_vote_count(action) == net_peer_count();
}
inline void house_vote_clear() { memset(house_votes, 0, sizeof(house_votes)); }
}
