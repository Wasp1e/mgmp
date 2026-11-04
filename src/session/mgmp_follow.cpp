// mgmp_follow.cpp -- see mgmp_follow.h.

#include "mgmp_catsync.h"
#include "mgmp_choice.h"
#include "mgmp_invsync.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_savefile.h"
#include "mgmp_follow.h"
#include "mgmp_lockstep.h"
#include "mgmp_meow.h"
#include "mgmp_net.h"
#include <cmath>
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace mgmp {
namespace {

// MapScreen's node vector, read off MapNode::Click and cross-checked against
// every other MapScreen function that walks it (update, generate_map,
// generate_auto_connections, generate_bonus_nodes, load_map, select_boss_level,
// TickNemesis all use the same +124/+128 pair). Standard CustomVector shape:
// { u32 capacity @ 0x78, u32 count @ 0x7C, MapNode** data @ 0x80 }.
constexpr uintptr_t kMap_NodeCount = 124;
constexpr uintptr_t kMap_NodeData  = 128;

// MapNode+0x118: the node's own 32-byte xoshiro256 seed, written by
// MapScreen::generate_map and copied into TLS+0x178 by EnterNode.
// MapNode+0x138: MapNodeType, from MapNode::str_to_type.
constexpr uintptr_t kNode_Seed = 0x118;
constexpr uintptr_t kNode_Type = 0x138;

// MapNode::str_to_type, in enum order. 1 and 18 are three-character names whose
// string constants IDA did not render; they are not battle types and nothing
// here depends on knowing them.
const char* node_type_name(uint32_t t) {
    static const char* kNames[] = {
        "none", "?1", "enter", "exit", "home", "battle", "hard", "miniboss",
        "boss", "event", "optional_event", "special_event", "shop", "treasure",
        "furniturebox", "foodbox", "bonus", "empty", "?18"
    };
    return t < (sizeof(kNames) / sizeof(kNames[0])) ? kNames[t] : "?";
}

struct State {
    bool     on             = false;
    bool     is_client      = false;

    void*    map            = nullptr;   // latest MapScreen*, from its update

    // NODES THE HOST HAS ENTERED AND THIS PEER HAS NOT -- A QUEUE, NOT A SLOT.
    //
    // It was one slot, and a second ENTERNODE overwrote it in silence. That is
    // not a race to tolerate: it is the client SKIPPING A WHOLE NODE of the
    // run, and it happened live on 2026-08-25. The host entered node 6 (an
    // event), resolved it and walked on to node 17 faster than this peer's map
    // tick came round; node 17 replaced node 6 in the slot; the client never
    // entered node 6, never built its option list, and never applied the event.
    // Its log says `<- host entered node 6` with no `following host into node
    // 6` after it, and there was no other line anywhere to say a node had been
    // dropped.
    //
    // The run was already wrong at that point. What made it unrecoverable was
    // that node 6's CHOICE stayed held and landed on node 3 two nodes later --
    // see ChoiceMsg::node_seed.
    //
    // A queue is the whole fix, because the peers do not have to be on the same
    // node at the same moment -- only to walk the same nodes in the same order.
    // The battle layer already assumes exactly that: the join barrier exists so
    // a peer can arrive at a fight late. Trailing the host by a node is the same
    // situation one screen earlier.
    static constexpr uint32_t kMaxPendingNodes = 8;
    struct PendingNode {
        uint32_t index = 0;
        uint32_t type  = 0;
        uint32_t count = 0;
        uint64_t seed  = 0;
        uint32_t run_id = 0;
        uint8_t  weather_count = 0;
        char     weather[kMaxNodeWeather][kNodeWeatherLen] = {};
        uint64_t at    = 0;         // tick the host's choice arrived
        bool     held  = false;     // ...and whether we have said we are holding
    };
    PendingNode pending_q[kMaxPendingNodes];
    uint32_t    pending_head  = 0;
    uint32_t    pending_count = 0;

    // The node this peer is standing in, remembered so a peer that connects
    // afterwards can be told where the run is. ENTERNODE used to be published
    // once, at the moment of entry, and nothing kept it -- so a reconnecting
    // peer loaded the save, landed on the map and waited forever for a message
    // that had already been sent. Measured 2026-08-26.
    bool     have_here      = false;
    uint32_t here_index     = 0;

    // HOST: names the current load of the run -- see battle_id_for.
    uint32_t run_id         = 0;

    // Last tick MapScreen::update ran. See follow_on_map: this is what tells
    // the save flush whether the run is between nodes or inside one.
    uint64_t map_tick       = 0;

    // CLIENT: see watch_edits.
    uint64_t edit_tick      = 0;
    uint64_t edit_base      = 0;
    uint64_t edit_seen      = 0;
    bool     edit_rebase    = true;
    bool     inv_open       = false;

    // THE VOTE. Both players pick the next node, as in Slay the Spire 2, and the
    // host walks only when the picks agree. -1 is "not picked".
    int32_t  host_vote      = -1;
    int32_t  client_vote    = -1;   // host: the client's; client: what it last sent
    bool     vote_go        = false;
    bool     vote_bypass    = false;
    // A swallowed EnterNode leaves the marker selected on that node; the next map
    // tick puts it back. Only the client's fallback still gets here.
    bool     unstick        = false;

    // The other player's pick, bobbing on this map. See blink_pick.
    int32_t  pick           = -1;
    void*    pick_graphic   = nullptr;
    float    pick_pos[2]    = {};

    // A jump asked for by the debug panel, applied at the tail of the next
    // MapScreen::update -- the same site the follow path enters nodes from.
    bool     jump_pending   = false;
    uint32_t jump_index     = 0;
    uint32_t here_count     = 0;
    uint32_t here_type      = 0;
    uint64_t here_seed      = 0;

    uint32_t entered        = 0;         // nodes followed, for the shutdown line
    uint32_t published      = 0;
    uint32_t suppressed     = 0;
    bool     warned_no_map  = false;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

// --- the pending-node queue -------------------------------------------------
//
// All three are called with the Guard held. `front` is only valid while
// `pending_any()` is true.

bool pending_any() { return g.pending_count != 0; }

State::PendingNode& pending_front() {
    return g.pending_q[g.pending_head];
}

void pending_pop() {
    if (!g.pending_count) return;
    g.pending_head = (g.pending_head + 1) % State::kMaxPendingNodes;
    --g.pending_count;
}

void pending_clear() { g.pending_head = 0; g.pending_count = 0; }

bool read_nodes(void* map, uint32_t& count, void*& data) {
    count = 0; data = nullptr;
    if (!map) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeCount, &count, sizeof(count))) return false;
    if (!mem_read((const uint8_t*)map + kMap_NodeData,  &data,  sizeof(data)))  return false;
    // A map with no nodes, or an implausible count, means we are reading the
    // wrong object -- say nothing rather than index into it.
    return data != nullptr && count > 0 && count < 4096;
}

void* node_at(void* map, uint32_t index) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data) || index >= count) return nullptr;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)data + index * sizeof(void*), &node, sizeof(node))) return nullptr;
    return node;
}

bool index_of_node(void* map, void* node, uint32_t& out) {
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map, count, data)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &p, sizeof(p))) return false;
        if (p == node) { out = i; return true; }
    }
    return false;
}

uint32_t node_type(void* node) {
    uint32_t t = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Type, &t, sizeof(t));
    return t;
}

uint64_t node_seed0(void* node) {
    uint64_t s = 0;
    if (node) mem_read((const uint8_t*)node + kNode_Seed, &s, sizeof(s));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

void follow_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on        = config().net_follow;
    g.is_client = (net_role() == NetRole::Client);
    g.map       = nullptr;
    pending_clear();
    if (!g.on) { log_line("FOLLOW", "map following disabled by net_follow = 0"); return; }
    log_line_lvl(LogLevel::Trace, "FOLLOW", "armed -- %s",
             g.is_client ? "following the host's map choices; local map input is suppressed"
                         : "publishing this peer's map choices");
    if (config().net_follow_delay_ms && g.is_client)
        log_line("FOLLOW", "!! TEST SETTING ACTIVE: net_follow_delay_ms = %u. This peer"
                           " will lag every node by that long ON PURPOSE, to exercise"
                           " the join barrier. Set it to 0 for a real session.",
                 config().net_follow_delay_ms);
    else if (config().net_follow_delay_ms)
        log_line("FOLLOW", "net_follow_delay_ms = %u is set but this peer is the host,"
                           " which never follows -- it will have no effect",
                 config().net_follow_delay_ms);
}

void follow_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "FOLLOW",
             "done: %u published, %u followed, %u local click(s) suppressed",
             g.published, g.entered, g.suppressed);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

bool follow_suppresses_local_input() { return g.on && g.is_client; }

// Where this peer is now. Recorded on BOTH peers -- the host so it can tell a
// joiner, the client so its own log says where it thinks it is.
void remember_node(uint32_t index, uint32_t count, uint32_t type, uint64_t seed) {
    const bool moved = !g.have_here || g.here_seed != seed;
    g.have_here  = true;
    g.here_index = index;
    g.here_count = count;
    g.here_type  = type;
    g.here_seed  = seed;

    // Tell the choice layer where the run now is. It holds a choice that has
    // not found its screen yet, and a choice belongs to exactly one node -- see
    // ChoiceMsg::node_seed for the run this cost.
    //
    // The seed is PUSHED rather than pulled. mgmp_choice could ask
    // follow_here_seed() when it applies, but that would have it take this
    // module's lock while holding its own, and this call already runs the other
    // way round -- the classic inversion. Handing the value over means neither
    // module ever waits on the other.
    if (moved) choice_on_node_entered(seed);

    // The meta layer's own hash, sampled at the one moment both peers reach
    // identically: immediately before EnterNode. Deliberately taken on BOTH
    // sides from the same function, because a hash whose two halves are
    // computed at different points in the frame compares two different things
    // and reports a mismatch that is entirely ours.
    //
    // Note this is BEFORE the publishes below, which is what makes the two
    // comparable: the host describes the run it is about to push, and the
    // client -- which applied that push when it popped, ahead of its own map
    // tick -- describes the same bytes.
    if (moved) nodehash_on_node(seed, index);
}

// --- the weather waiting for the next fight ---------------------------------
//
// Measured 2026-11-01: `Happening` rolled weather on the host, and the next
// battle opened with the rng apart and every cat equal. The list is not in any
// push, and neither peer has to agree on how it got there, only on what it holds
// when the battle reads it.

struct GameString {            // MSVC std::string, short form
    char     buf[16] = {};
    uint64_t size    = 0;
    uint64_t cap     = 15;
};
static_assert(sizeof(GameString) == 32, "the list's element size");

static const uint8_t* weather_director() {
    const void** slot = (const void**)addr_of_data(D_MewDirectorPtr);
    const void* dir = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir))) return nullptr;
    return (const uint8_t*)dir;
}

static uint32_t weather_read(char out[kMaxNodeWeather][kNodeWeatherLen]) {
    const uint8_t* dir = weather_director();
    if (!dir) return 0;
    const uint8_t* begin = nullptr;
    const uint8_t* end   = nullptr;
    if (!mem_read(dir + kDir_WeatherBegin, &begin, sizeof(begin)) ||
        !mem_read(dir + kDir_WeatherEnd,   &end,   sizeof(end)) || !begin || end <= begin)
        return 0;

    const uint32_t n = (uint32_t)((end - begin) / sizeof(GameString));
    if (n > kMaxNodeWeather)
        log_line("FOLLOW", "!! %u weathers queued, only %u can be sent", n, kMaxNodeWeather);

    uint32_t kept = 0;
    for (uint32_t i = 0; i < n && kept < kMaxNodeWeather; ++i) {
        GameString s;
        if (!mem_read(begin + i * sizeof(GameString), &s, sizeof(s))) return kept;
        if (s.cap > 15 || s.size > 15) {
            log_line("FOLLOW", "!! a weather name is too long to send");
            continue;
        }
        memcpy(out[kept], s.buf, 16);
        out[kept][15] = 0;
        ++kept;
    }
    return kept;
}

static void weather_write(EnterNodeMsg& m) {
    m.weather_count = (uint8_t)weather_read(m.weather);
}

// CLIENT, on the map, just before the node.
static void weather_apply(uint32_t count, const char want[kMaxNodeWeather][kNodeWeatherLen]) {
    char have[kMaxNodeWeather][kNodeWeatherLen] = {};
    const uint32_t n = weather_read(have);
    if (n == count && !memcmp(have, want, count * kNodeWeatherLen)) return;

    log_line("FOLLOW", "weather: the host has %u (first '%s'), this peer %u (first '%s')"
                       " -- taking the host's", count, count ? want[0] : "-",
             n, n ? have[0] : "-");

    typedef void (__fastcall *fn_emplace)(void*, void*);
    fn_emplace emplace = (fn_emplace)addr_of_call(C_StringVecEmplace);
    const uint8_t* dir = weather_director();
    if (!emplace || !dir) {
        log_line("FOLLOW", "!! cannot reach the weather list");
        return;
    }

    // Rewound rather than destructed: the names are all in the short form, so
    // there is nothing behind them to free.
    uint8_t* vec = (uint8_t*)dir + kDir_WeatherBegin;
    const uint8_t* begin = nullptr;
    if (!mem_read(vec, &begin, sizeof(begin))) return;
    mem_write(vec + (kDir_WeatherEnd - kDir_WeatherBegin), &begin, sizeof(begin));

    for (uint32_t i = 0; i < count; ++i) {
        GameString s;
        memcpy(s.buf, want[i], kNodeWeatherLen);
        s.size = strlen(s.buf);
        emplace(vec, &s);
    }
}

static void take_client_edit();
static void unstick_marker(void* map_screen);
static void show_pick(int32_t index);
static void blink_pick(void* map_screen);

bool follow_on_enter_node(void* map_screen, void* node, bool* sent) {
    if (sent) *sent = false;
    if (!g.on || !net_active() || !map_screen || !node) return true;

    Guard guard;
    g.map = map_screen;                    // the click proves the pointer is live

    uint32_t index = 0;
    bool     known = index_of_node(map_screen, node, index);
    uint32_t type  = node_type(node);

    // --- the client: swallow it ------------------------------------------
    //
    // The host owns the run, so a click here is a second opinion nobody asked
    // for -- and acting on it would put the two peers on different nodes, which
    // no amount of battle-layer lockstep can recover from. Swallowing rather
    // than disabling the UI keeps the change to one function.
    //
    // Swallowed, but now a vote: the host goes only where both players clicked.
    if (g.is_client) {
        ++g.suppressed;
        if (known && g.client_vote != (int32_t)index) {
            g.client_vote = (int32_t)index;
            ChoiceMsg v{};
            v.kind      = kChoiceNode;
            v.index     = index;
            v.node_seed = g.have_here ? g.here_seed : 0;
            net_send_choice(v);
            log_line("FOLLOW", "this player picked node %u (%s) -- waiting for the host",
                     index, node_type_name(type));
        }
        g.unstick = true;
        return false;
    }

    // --- the host: publish it --------------------------------------------
    if (!known) {
        // Nothing to send that the client could resolve. Do not block the host
        // over it: a run that continues single-player is better than one that
        // stops, and the client will notice it is on a different node the
        // moment a battle starts and the hashes disagree.
        log_line("FOLLOW", "!! entered a node that is not in MapScreen's vector"
                           " -- cannot publish it; the client will not follow");
        return true;
    }

    g.host_vote   = -1;
    g.client_vote = -1;
    g.vote_go     = false;
    show_pick(-1);

    uint32_t count = 0; void* data = nullptr;
    read_nodes(map_screen, count, data);

    // Ahead of everything below: save, then cats and inventory, then the node.
    savefile_on_leaving_house();

    // Battle identity first of all, and before the publish: from here on this
    // peer's ACTION/HASH/CONTROL name this node, and the client establishes the
    // same id from the same field of the same node in follow_map_update.
    const uint64_t seed = node_seed0(node);
    lockstep_enter_battle(seed, g.run_id);
    remember_node(index, count, type, seed);

    // Cats FIRST, node second. TCP is ordered and the client applies a CATDATA
    // the moment it pops, while it defers the node entry to the tail of
    // MapScreen::update -- so sending in this order is what guarantees the
    // battle is built from the host's cats rather than from the client's stale
    // ones. Reversing these two lines reintroduces the bug this closes.
    //
    // A click on the map means no inventory screen is open here, which makes this
    // the one safe moment for what the client edited: applied before the publish,
    // so a refused edit is rolled back by it and the client ends up with the host's.
    take_client_edit();

    // Everything, not just what changed HERE. The dedupe compares against what
    // this peer last sent, so it cannot see a client that changed its own copy,
    // and the client's shop is live: on 2026-10-31 it bought a level-up the host
    // never saw, kept the cat, and could walk further than the host allowed.
    // The inventory buckets drift the same way. A node is rare, so a few KB here
    // is cheap, and the client ends every node holding exactly the host's run.
    catsync_forget();
    invsync_forget();
    catsync_publish("entering a node");
    // ...and the other half of the same equip. Order between these two does not
    // matter the way it does against ENTERNODE -- an equip has already moved
    // the item out of the inventory and onto the cat by the time either is
    // serialized, so neither push depends on the other -- but both must land
    // BEFORE the node, and the client applies each the moment it pops.
    invsync_publish("entering a node");
    // ...and the used-event list, which is the third piece of run state and the
    // one nothing pushed until now. Same ordering requirement as the other two:
    // BEFORE the node, because it is what the next event will be rolled from.
    runhist_publish("entering a node");

    EnterNodeMsg m{};
    m.index      = index;
    m.node_count = count;
    m.type       = type;
    m.seed0      = seed;
    m.run_id     = g.run_id;
    lockstep_owners_write(m);
    weather_write(m);
    if (net_send_enter_node(m)) {
        ++g.published;
        if (sent) *sent = true;
        log_line("FOLLOW", "-> node %u/%u (%s) seed0=%016llx",
                 index, count, node_type_name(type), (unsigned long long)m.seed0);
    }
    return true;
}

// A peer connected or reconnected: tell it which node the run is standing in.
//
// Without this the whole save/cats/inventory catch-up leaves the joiner on the
// map with nothing to do, because ENTERNODE is otherwise published exactly once
// -- at the instant of entry -- and a peer that was not connected then never
// hears about it. That was the "client loads the save and the battle never
// activates" report.
bool follow_on_map() {
    if (!g.map_tick) return false;
    // 250 ms: MapScreen::update does not tick on the frame a node is entered,
    // and a single missed frame is not a reason to refuse a flush. A quarter
    // second is far shorter than any node and far longer than any frame.
    return (GetTickCount64() - g.map_tick) < 250;
}

uint32_t follow_node_count() {
    if (!g.map) return 0;
    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(g.map, count, data)) return 0;
    return count;
}

bool follow_node_info(uint32_t index, uint32_t& type, uint64_t& seed0,
                      const char** type_name) {
    if (!g.map) return false;
    void* node = node_at(g.map, index);
    if (!node) return false;
    type  = node_type(node);
    seed0 = node_seed0(node);
    if (type_name) *type_name = node_type_name(type);
    return true;
}

bool follow_current_node(uint32_t& index) {
    if (!g.have_here) return false;
    index = g.here_index;
    return true;
}

// Deliberately unlocked. It is one naturally-aligned u64 written by the game
// thread in remember_node and read by the game thread when the host stamps a
// CHOICE, so the lock would only buy a lock-ordering hazard against the module
// that wants the value. See remember_node for the other half of that argument.
uint64_t follow_here_seed() { return g.have_here ? g.here_seed : 0; }

// --- where the run is standing, read from the GAME rather than remembered ----
//
// follow_current_node above reports the node this MODULE watched somebody
// enter. That is the wrong answer for the case that matters most: a run
// reloaded from disk has entered nothing this session, so it reports nothing --
// and a reloaded run parked on an unresolved node is exactly when you need to
// be told which node that is.
//
// glaiel::MapNode::Click sets the current node with
//
//     *(MapNode+0x170 -> MapScreen) + 0xA0) + 0x60 = node
//
// and guards its own entry on the same read. MapScreen+0xA0 is the map MARKER
// (sub_14038DE60 calls MapMarker::CanReachNode on it and derives a facing from
// it), and it carries two node slots: +0x50, which is where the marker IS, and
// +0x60, which is what Click selected.
//
// BOTH ARE VALIDATED AGAINST THE NODE VECTOR RATHER THAN TRUSTED. index_of_node
// only reports an index when the pointer is genuinely an element of
// MapScreen+0x80, so a wrong offset yields "unknown" instead of a confident
// wrong number -- the same rule the per-turn state hash follows. That is what
// makes reading two undocumented slots an acceptable risk here.
constexpr uintptr_t kMap_Marker      = 0xA0;
constexpr uintptr_t kMarker_AtNode   = 0x50;
constexpr uintptr_t kMarker_Selected = 0x60;

bool read_marker_node(uintptr_t slot, uint32_t& index) {
    if (!g.map) return false;
    const void* marker = nullptr;
    if (!mem_read((const uint8_t*)g.map + kMap_Marker, &marker, sizeof(marker)) || !marker)
        return false;
    void* node = nullptr;
    if (!mem_read((const uint8_t*)marker + slot, &node, sizeof(node)) || !node)
        return false;
    return index_of_node(g.map, node, index);
}

// Where a node is drawn, and where the marker keeps its own position. The map's
// load path places the marker from exactly these (0x140228AF7). The cats trailing
// the marker read its position every frame, so they run after it on their own.
constexpr uintptr_t kNode_Graphic    = 0x160;
constexpr uintptr_t kGraphic_Pos     = 0x70;    // float x, y
constexpr uintptr_t kMarker_Object   = 0x48;
constexpr uintptr_t kObject_Pos      = 0x80;    // double x, y, z

// The client enters a node straight from the host's word, so nothing walks its
// marker there and the map stayed on the first node for the whole run. Every
// check here fails to doing nothing: the marker is scenery to the battle, and
// what a reload reads back is this same slot.
void place_marker(void* map_screen, void* node) {
    uint8_t* marker = nullptr;
    if (!mem_read((const uint8_t*)map_screen + kMap_Marker, &marker, sizeof(marker)) || !marker)
        return;

    const uint8_t* graphic = nullptr;
    uint8_t*       object  = nullptr;
    float          pos[2]  = {};
    if (!mem_read((const uint8_t*)node + kNode_Graphic, &graphic, sizeof(graphic)) || !graphic ||
        !mem_read(graphic + kGraphic_Pos, pos, sizeof(pos)) ||
        !mem_read(marker + kMarker_Object, &object, sizeof(object)) || !object) {
        log_line("FOLLOW", "!! could not move the map marker to the host's node");
        return;
    }

    mem_write(marker + kMarker_AtNode,   &node, sizeof(node));
    mem_write(marker + kMarker_Selected, &node, sizeof(node));
    const double xyz[3] = { pos[0], pos[1], 0.0 };
    mem_write(object + kObject_Pos, xyz, sizeof(xyz));
}

// Back to the node the run is standing in, selection included, so a click on any
// node is taken again. That node is the last one entered; without one, the
// marker's own.
static void unstick_marker(void* map_screen) {
    void* at = g.have_here ? node_at(map_screen, g.here_index) : nullptr;
    if (!at) {
        uint8_t* marker = nullptr;
        if (mem_read((const uint8_t*)map_screen + kMap_Marker, &marker, sizeof(marker)) && marker)
            mem_read(marker + kMarker_AtNode, &at, sizeof(at));
    }
    if (at) place_marker(map_screen, at);
}

bool follow_marker_node(uint32_t& index)   { return read_marker_node(kMarker_AtNode,   index); }
bool follow_selected_node(uint32_t& index) { return read_marker_node(kMarker_Selected, index); }

void follow_request_jump(uint32_t index) {
    g.jump_pending = true;
    g.jump_index   = index;
}

// The clock rather than a counter: a host whose process restarted would count
// from one again and reuse an id a connected client has already played.
void follow_on_run_restart() {
    Guard guard;
    g.run_id    = (uint32_t)time(nullptr);
    g.have_here = false;      // the node we were in belongs to the run just left
    lockstep_on_run_restart();
    log_line("FOLLOW", "new load of the run: %08x", g.run_id);
}

// THE OTHER PLAYER'S PICK, SHOWN ON THE NODE ITSELF. A frame drawn where its
// pointer was came out a node's height off on 2026-11-03: the two map views were
// scrolled differently, and the mod has no camera to correct by. A fade did
// nothing, the picture is a swf::MovieClip and not a Renderer. Its position is
// known, though, MapNode+0x160 -> +0x70, the one place_marker reads, so the node
// bobs up and down in this player's own camera and goes back when the pick ends.
static void restore_pick() {
    if (!g.pick_graphic) return;
    mem_write((uint8_t*)g.pick_graphic + kGraphic_Pos, g.pick_pos, sizeof(g.pick_pos));
    g.pick_graphic = nullptr;
}

static void show_pick(int32_t index) {
    if (index == g.pick) return;
    restore_pick();
    g.pick = index;
}

static void blink_pick(void* map_screen) {
    if (g.pick < 0) return;
    void* node    = node_at(map_screen, (uint32_t)g.pick);
    void* graphic = nullptr;
    if (!node || !mem_read((const uint8_t*)node + kNode_Graphic, &graphic, sizeof(graphic)) ||
        !graphic)
        return;
    if (graphic != g.pick_graphic) {
        restore_pick();
        if (!mem_read((const uint8_t*)graphic + kGraphic_Pos, g.pick_pos, sizeof(g.pick_pos)))
            return;
        g.pick_graphic = graphic;
    }
    constexpr double kPeriodMs = 700.0;
    constexpr float  kHop      = 10.0f;
    const double t = (double)(GetTickCount64() % (uint64_t)kPeriodMs) / kPeriodMs;
    const float  y = g.pick_pos[1] - kHop * (float)sin(t * 3.14159265358979);
    const float  p[2] = { g.pick_pos[0], y };
    mem_write((uint8_t*)graphic + kGraphic_Pos, p, sizeof(p));
}

// THE VOTE IS TAKEN AT THE CLICK. Holding EnterNode instead let the game select the
// node and walk the marker, and then the map took no other click: two different
// picks could not be changed by either player, measured twice on 2026-11-03. The
// node buttons' std::function target is sub_140228700, {vftable, MapNode*}; not
// running it leaves the map exactly as it was, so the player can click again.
//
// Client: never runs, the pick goes to the host. Host: runs once the other player
// picked the same node, or when the host clicks its own pick a second time.
constexpr uintptr_t kRva_NodeClickSlot = 0xF00620;   // _Do_call of that _Func_impl
constexpr uintptr_t kRva_NodeClick     = 0x228700;

using fn_node_click = void (__fastcall*)(void*, void*, void*, void*);
fn_node_click o_node_click = nullptr;

static void send_vote(uint32_t index) {
    ChoiceMsg v{};
    v.kind      = kChoiceNode;
    v.index     = index;
    v.node_seed = g.have_here ? g.here_seed : 0;
    net_send_choice(v);
}

static bool vote_click(void* node) {
    if (!g.on || !net_active() || !g.map || !node) return true;
    Guard guard;
    uint32_t index = 0;
    if (!index_of_node(g.map, node, index)) return true;
    const char* type = node_type_name(node_type(node));

    // EVERY click is sent and meows, a repeat included: the meow is how each
    // player hears the other one clicking, not a sound for a pick that changed.
    // A repeated vote changes nothing at the other end -- the same index again.
    if (g.is_client) {
        send_vote(index);
        meow_play();
        if (g.client_vote != (int32_t)index) {
            g.client_vote = (int32_t)index;
            log_line("FOLLOW", "this player picked node %u (%s) -- waiting for the host",
                     index, type);
        }
        return false;
    }
    if (g.vote_bypass || net_peer_count() < 2) return true;

    const bool same = g.host_vote == (int32_t)index;
    g.host_vote = (int32_t)index;
    send_vote(index);
    meow_play();
    if (g.client_vote == (int32_t)index) {
        log_line("FOLLOW", "both players picked node %u -- going", index);
        return true;
    }
    if (same) {
        log_line("FOLLOW", "the host clicked node %u again -- going without the other"
                           " player's pick", index);
        return true;
    }
    log_line("FOLLOW", "the host picked node %u (%s) -- waiting for the other player to"
                       " pick it too; click it again to go alone", index, type);
    return false;
}

static void __fastcall node_click(void* closure, void* b, void* c, void* d) {
    void* node = nullptr;
    mem_read((const uint8_t*)closure + 8, &node, sizeof(node));
    if (vote_click(node)) o_node_click(closure, b, c, d);
}

void follow_set_base(uintptr_t base) {
    if (o_node_click) return;
    void** slot = (void**)(base + kRva_NodeClickSlot);
    void*  have = nullptr;
    DWORD  prot = 0;
    if (!mem_read(slot, &have, sizeof(have)) || (uintptr_t)have != base + kRva_NodeClick ||
        !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
        log_line("FOLLOW", "!! the map node click moved -- the map vote is off and the"
                           " host walks alone");
        return;
    }
    o_node_click = (fn_node_click)have;
    *slot = (void*)&node_click;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
}

void follow_on_vote(const ChoiceMsg& m) {
    if (!g.on) return;
    Guard guard;
    // A pick made on another node is about another map position.
    if (m.node_seed && g.have_here && m.node_seed != g.here_seed) return;
    show_pick((int32_t)m.index);
    meow_play();

    if (g.is_client) {
        log_line("FOLLOW", "the host picked node %u%s", m.index,
                 g.client_vote == (int32_t)m.index ? " -- the same as this player"
                                                   : " -- click it too to go there");
        return;
    }
    g.client_vote = (int32_t)m.index;
    if (g.host_vote == g.client_vote) {
        g.vote_go = true;
        log_line("FOLLOW", "both players picked node %u -- going", m.index);
    } else {
        log_line("FOLLOW", "the other player picked node %u%s", m.index,
                 g.host_vote < 0 ? "" : " -- not the host's pick");
    }
}

void follow_catchup(uint8_t peer) {
    if (!g.on || g.is_client || !net_active()) return;

    Guard guard;
    if (!g.have_here) {
        log_line("FOLLOW", "peer %u joined before this peer entered any node --"
                           " nothing to catch it up to; the next node is published"
                           " normally", (unsigned)peer);
        return;
    }

    EnterNodeMsg m{};
    m.index      = g.here_index;
    m.node_count = g.here_count;
    m.type       = g.here_type;
    m.seed0      = g.here_seed;
    m.run_id     = g.run_id;
    lockstep_owners_write(m);
    weather_write(m);
    if (!net_send_enter_node_to(peer, m)) return;

    log_line("FOLLOW", "-> re-sent the CURRENT node %u/%u (%s) seed0=%016llx to"
                       " peer %u (it joined after we entered it)",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0, (unsigned)peer);
}

void follow_on_message(const EnterNodeMsg& m) {
    if (!g.on) return;
    Guard guard;
    if (!g.is_client) {
        // Symmetric message, asymmetric authority: a host that receives one is
        // talking to a peer that thinks it is the host too.
        log_line("FOLLOW", "!! received a node choice while hosting -- both peers"
                           " believe they own the run");
        return;
    }
    lockstep_owners_read(m);
    // A full queue means this peer is eight nodes behind, which no amount of
    // waiting is going to fix. Refuse the newest and SAY SO: the run is already
    // wrong, and the one thing that must not happen is what used to -- losing a
    // node without a line in the log. Dropping the newest rather than the
    // oldest keeps the queue contiguous, the same rule the peer-hash ring
    // follows for the same reason.
    if (g.pending_count >= State::kMaxPendingNodes) {
        log_line("FOLLOW", "!! %u nodes already queued and the host entered another"
                           " (node %u) -- REFUSING it. This peer is too far behind"
                           " to follow the run and the two are no longer the same"
                           " run.",
                 g.pending_count, m.index);
        choice_on_node_skipped(m.seed0);
        return;
    }

    State::PendingNode& p =
        g.pending_q[(g.pending_head + g.pending_count) % State::kMaxPendingNodes];
    ++g.pending_count;
    p.index = m.index;
    p.type  = m.type;
    p.count = m.node_count;
    p.seed  = m.seed0;
    p.run_id = m.run_id;
    p.weather_count = m.weather_count;
    memcpy(p.weather, m.weather, sizeof(p.weather));
    // Timed from ARRIVAL, not from the first map update that sees it: the point
    // of the delay is to put a known gap between the host entering a node and
    // this peer entering it, and the map screen may not be ready for a while
    // either way.
    p.at    = GetTickCount64();
    p.held  = false;
    log_line("FOLLOW", "<- host entered node %u/%u (%s) seed0=%016llx%s",
             m.index, m.node_count, node_type_name(m.type),
             (unsigned long long)m.seed0,
             g.pending_count > 1 ? " -- QUEUED, this peer is still behind" : "");
}

// CLIENT: did the player change equipment since the last thing that came from the
// host? A battle and a host's push both change it too, so each moves the baseline
// instead. An item being dragged is in neither the bag nor a slot, so a change has
// to hold still for one more look before it goes out.
static void take_client_edit() {
    const int edit = invsync_take_edit();
    if (!edit) return;
    catsync_take_equipment(edit > 0);
    if (edit > 0) {
        catsync_mark_sent();
        log_line("FOLLOW", edit == 1 ? "took the client's equipment edit"
                                     : "took the client's equipment, its bag matched");
        return;
    }
    log_line("FOLLOW", "!! refused the client's equipment edit -- the host changed the"
                       " inventory first");
    catsync_forget();
    invsync_forget();
    catsync_publish("a refused edit");
    invsync_publish("a refused edit");
}

// CLIENT, with no inventory screen up. A push held over a screen is older than
// whatever the player did on that screen: applied now, the bag would get back an
// item the player has since put on a cat, and the cat would keep it. The user saw
// items in both on 2026-11-03. So an unsent edit goes out instead,
// with the base this peer really holds. The host sees it missed its change, refuses
// it and sends everything again, and both halves arrive together.
static void take_host_push() {
    if (!catsync_has_pending() && !invsync_has_pending()) return;

    const uint64_t sig = catsync_equipment_sig() ^ invsync_local_hash();
    if (!g.edit_rebase && sig && sig != g.edit_base) {
        log_line("FOLLOW", "the host changed something during this player's equipment"
                           " edit -- asking it to settle that");
        invsync_drop_pending();
        catsync_drop_pending();
        g.edit_base = sig;
        g.edit_seen = sig;
        catsync_send_equipment();
        invsync_send_edit();
        return;
    }
    catsync_apply_pending("on the map");
    invsync_apply_pending("on the map");
    g.edit_rebase = true;
}

static void watch_edits() {
    const uint64_t sig = catsync_equipment_sig() ^ invsync_local_hash();
    if (g.edit_rebase) {
        g.edit_rebase = false;
        g.edit_base   = sig;
        g.edit_seen   = sig;
        return;
    }
    if (!sig || sig == g.edit_base) { g.edit_seen = sig; return; }
    if (sig != g.edit_seen) { g.edit_seen = sig; return; }
    g.edit_base = sig;
    log_line("FOLLOW", "this player changed the equipment -- sending it to the host");
    catsync_send_equipment();
    invsync_send_edit();
}

// Once a second, on the map or in a shop: the two places equipment changes hands
// with nothing else running. A battle would write cats its Characters derive from.
// Each publish sends only what moved, so a chest or a host's equip reaches the
// client now instead of at the next node. Order matters on the host: its own change
// goes out first, so the edit it now takes is judged against it.
void follow_edit_pump() {
    constexpr uint64_t kEveryMs = 1000;
    if (!g.on) return;
    Guard guard;
    const uint64_t now = GetTickCount64();
    if (now - g.edit_tick < kEveryMs) return;
    g.edit_tick = now;

    const bool in_shop = g.have_here && strcmp(node_type_name(g.here_type), "shop") == 0;
    if (!follow_on_map() && !in_shop) return;

    // Everything here trusts this reading, and a wrong "closed" would free items
    // under a live screen, so the log has to show it working.
    const bool open = invsync_screen_open();
    if (open != g.inv_open) {
        g.inv_open = open;
        log_line("FOLLOW", "inventory screen %s", open ? "up" : "down");
    }

    if (g.is_client) {
        if (pending_any()) return;
        if (!invsync_screen_open()) take_host_push();
        watch_edits();
    } else {
        catsync_publish("changed on the map");
        invsync_publish("changed on the map");
        if (!invsync_screen_open()) take_client_edit();
    }
}

void* follow_map_update(void* map_screen) {
    if (!g.on || !map_screen) return nullptr;

    Guard guard;
    g.map = map_screen;

    // The map is ticking, so the run is BETWEEN nodes. This is the stamp the
    // save flush reads -- see follow_on_map.
    g.map_tick = GetTickCount64();
    if (g.unstick) { g.unstick = false; unstick_marker(map_screen); }
    blink_pick(map_screen);


    // --- a jump asked for by the debug panel --------------------------------
    //
    // Handled before the follow path and returning early, because the two are
    // different intentions and letting a jump fall through into the pending
    // follow would enter two nodes on one tick.
    // --- both players picked the same node ---------------------------------
    //
    // Entered here rather than from the message: the host's own click was held, and
    // the map screen is the one place EnterNode is called from. Same shape as the
    // panel jump below.
    if (g.vote_go && !g.is_client) {
        g.vote_go = false;
        void* node = g.host_vote >= 0 ? node_at(map_screen, (uint32_t)g.host_vote) : nullptr;
        if (!node) return nullptr;
        bool sent = false;
        g.vote_bypass = true;
        const bool go = follow_on_enter_node(map_screen, node, &sent);
        g.vote_bypass = false;
        if (go) place_marker(map_screen, node);
        return go ? node : nullptr;
    }

    if (g.jump_pending) {
        g.jump_pending = false;
        const uint32_t index = g.jump_index;

        uint32_t count = 0; void* data = nullptr;
        if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready

        void* node = node_at(map_screen, index);
        if (!node) {
            log_line("FOLLOW", "!! panel asked to enter node %u but this map has"
                               " %u node(s) -- ignored", index, count);
            return nullptr;
        }

        // Loud, unconditionally, and on both peers' behalf: this is the debug
        // panel EDITING THE RUN. Everything else in this module reacts to a
        // decision a player made by clicking the map.
        log_line("FOLLOW", "!! PANEL JUMP: entering node %u (%s) seed0=%016llx by"
                           " request from the debug panel -- this is not a move the"
                           " player made on the map",
                 index, node_type_name(node_type(node)),
                 (unsigned long long)node_seed0(node));

        // Mirror h_EnterNode exactly: publish through the ordinary path first,
        // then let the caller run the original. Calling o_EnterNode alone would
        // bypass the detour and the client would never hear about it -- the same
        // property the follow path relies on in the other direction.
        bool sent = false;
        if (!follow_on_enter_node(map_screen, node, &sent)) return nullptr;
        return node;
    }

    if (!pending_any()) return nullptr;
    State::PendingNode& p = pending_front();

    // --- the deliberate late-join delay (test knob) -------------------------
    //
    // Holding the node here rather than dropping it is what makes this a test
    // of the join barrier and not just a broken client: the host walks into the
    // battle alone, this peer arrives net_follow_delay_ms later, and the barrier
    // is what has to have kept the host from playing turns in the meantime.
    // Everything downstream -- the seed check, the type check -- still runs when
    // the hold expires, so the delay changes WHEN we follow and nothing else.
    if (const uint32_t delay = config().net_follow_delay_ms) {
        const uint64_t waited = GetTickCount64() - p.at;
        if (waited < delay) {
            if (!p.held) {
                p.held = true;
                log_line("FOLLOW", "!! net_follow_delay_ms = %u -- deliberately holding"
                                   " node %u for %u ms before following. This is a TEST"
                                   " setting: it manufactures the late-join gap.",
                         delay, p.index, delay);
            }
            return nullptr;
        }
        if (p.held)
            log_line("FOLLOW", "delay elapsed (%llu ms) -- following now",
                     (unsigned long long)waited);
    }

    uint32_t count = 0; void* data = nullptr;
    if (!read_nodes(map_screen, count, data)) return nullptr;   // not ready yet

    // Same map, or the index means nothing. Both peers generate the map from
    // the same save, so a differing count is a real problem and not a race.
    if (p.count && count != p.count) {
        log_line("FOLLOW", "!! host's map has %u nodes, ours has %u -- not"
                           " following; the two runs are not the same run",
                 p.count, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    void* node = node_at(map_screen, p.index);
    if (!node) {
        log_line("FOLLOW", "!! node %u is out of range on our map (%u nodes)",
                 p.index, count);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    // Identity check, and it is the strong one: the node's stored seed is the
    // 32 bytes EnterNode is about to load into the simulation stream, so if it
    // differs the battle would start from a different RNG state and desync on
    // its first roll. Catching it here names the cause; catching it at the
    // first turn hash would only say "they disagree".
    uint64_t seed = node_seed0(node);
    if (p.seed && seed != p.seed) {
        log_line("FOLLOW", "!! node %u seed is %016llx here but %016llx on the"
                           " host -- refusing to enter; the maps differ",
                 p.index, (unsigned long long)seed,
                 (unsigned long long)p.seed);
        const uint64_t lost = p.seed;
        pending_pop();
        choice_on_node_skipped(lost);
        return nullptr;
    }

    uint32_t type = node_type(node);
    if (p.type != type)
        log_line("FOLLOW", "!! node %u is '%s' here but '%s' on the host --"
                           " entering anyway, the seed matched",
                 p.index, node_type_name(type), node_type_name(p.type));

    const uint32_t entering = p.index;
    const uint32_t run_id   = p.run_id;
    weather_apply(p.weather_count, p.weather);
    pending_pop();
    ++g.entered;
    log_line("FOLLOW", "following host into node %u (%s)%s",
             entering, node_type_name(type),
             pending_any() ? " -- more nodes still queued" : "");
    g.client_vote = -1;
    show_pick(-1);
    g.edit_rebase = true;

    // Battle identity, established without negotiating anything: this is the
    // same node the host entered, so `seed` is the same 64 bits it read. The
    // host does the matching call in follow_on_enter_node. Note this is the
    // CLIENT's only chance to make it -- the follow path calls o_EnterNode, the
    // MinHook trampoline, which bypasses the h_EnterNode detour entirely.
    lockstep_enter_battle(seed, run_id);

    // Everything the host published for this node lands HERE, and it must land
    // BEFORE remember_node, which is where the meta hash is taken.
    //
    // Two separate reasons, and both were measured wrong on 2026-08-26.
    //
    // The apply point itself: this is the last moment before EnterNode, so it
    // is still in time for the battle or shop the node is about to open; and it
    // is inside MapScreen::update, so the inventory apply's clear-and-rebuild
    // cannot free Equipment out from under an inventory screen this peer had
    // open when the host clicked. See the note above invsync_apply_pending.
    //
    // The ORDER: the host takes its node hash after publishing, so the hash
    // describes the run it just pushed. With these calls below remember_node
    // the client hashed the run it had BEFORE the push -- one node stale --
    // and reported an inventory mismatch that was entirely ours. That is
    // exactly the "a hash whose two halves are computed at different points in
    // the frame compares two different things" failure remember_node's own
    // comment warns about.
    invsync_apply_pending("about to follow the host into a node");
    catsync_apply_pending("about to follow the host into a node");
    runhist_apply_pending("about to follow the host into a node");

    remember_node(entering, count, type, seed);
    place_marker(map_screen, node);

    // A shift-held EnterNode takes a completely different branch (the very
    // first thing it does is SDL_GetScancodeFromKey(SDLK_LSHIFT) and, if down,
    // tail-calls sub_1403923F0 instead). That is local keyboard state, so the
    // client would diverge from the host purely by holding a key. Cheap to
    // notice, so say so rather than let it be mysterious.
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        log_line("FOLLOW", "!! LEFT SHIFT is down -- EnterNode branches on it and"
                           " this peer will not take the host's path");

    return node;
}

} // namespace mgmp
