#include "mgmp_startrun.h"
#include "mgmp_cutscene.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_ui.h"
#include "mgmp_resolve.h"
#include "mgmp_addresses.h"
#include "MinHook.h"
#include "mgmp_config.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <intrin.h>

#include <windows.h>

namespace mgmp {
namespace {

// Read off Mewgenics.exe 1.1.21239 on 2026-11-03, statically. SwfCutscene
// (vtable 0x111FCA8) and DialogController (0xF08EA8) both override slot 10, the
// per-frame update, so the last frame either ran says whether one is up. The
// input itself is not touched here: every reader in the game goes through
// sub_140985AF0 / sub_140985A90 on bindings fed by SDL's window messages, so a
// message posted to the window is a click to all of them.
constexpr uintptr_t kRva_SwfVtable    = 0x111FCA8;
constexpr uintptr_t kRva_SwfUpdate    = 0x829A00;
constexpr uintptr_t kRva_DialogVtable = 0xF08EA8;
constexpr uintptr_t kRva_DialogUpdate = 0x2D7030;
constexpr uint32_t  kUpdateSlot       = 10;
constexpr uint64_t  kGoneMs           = 300;
constexpr uint64_t  kStaleInputMs     = 5000;
constexpr uint64_t  kHostUpEveryMs    = 1000;   // the host says it is in one this often
constexpr uint64_t  kHostUpForMs      = 3000;   // ...and the client believes it this long
constexpr uint32_t  kHostUp           = 0xFE;   // aux: not an input, the host's state
constexpr uint32_t  kHostDown         = 0xFF;

using fn_update = void* (__fastcall*)(void*, void*, void*, void*);

// THE START. sub_140827ED0 is what every cutscene goes through -- the House,
// events, NPCs, the map: (std::string name, std::function on_start,
// std::function on_end, bool, bool), all BY VALUE, the callee destroys all
// three. The host's are sent; the client plays one itself when its own game did
// not start the same name around the same time. Its callbacks stay empty: what
// the host's do next (a node, a reload) reaches the client the usual way.
constexpr uintptr_t kRva_PlayCutscene = 0x827ED0;
const uint8_t kPlayPrologue[8] = { 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54 };
constexpr uint64_t kOwnWindowMs  = 3000;    // the client's own start counts this close

// THE SECOND PLAYER'S OWN CUTSCENES ARE SKIPPED. A rejoin, a day change, any
// reload takes the client through the menu, and the game plays its opening
// there every time. SwfCutscene ends when its hold-to-skip progress at +0x128
// reaches 1.0 (update, 0x829B12), so that is written. Only one the host is not
// watching: one the host is in, or started under the same name, plays out.
constexpr uintptr_t kSwf_SkipProgress = 0x128;
constexpr uint64_t  kSkipAfterMs      = 2500;  // the host's word has this long to arrive
// Never the host's to watch with the client: the opening the menu plays. On
// 2026-11-04 it ran on the client while the host sat in the day's-end dialog,
// and was kept because the host was "in a cutscene".
const char* const kAlwaysSkip[] = { "game_intro" };
constexpr uint64_t  kHostStartMs      = 10000;
constexpr uint64_t kStartStaleMs = 30000;   // a start the client never got to

// NPC OFFICES. A cat sent down the House pipe, or an NPC clicked on the House
// map, takes the game to that NPC's office (Frank's attic upgrade and the like):
// NPCMapDrawer::GoToNPCOffice(this, npc) at 0x27CE70, npc 0 beanies .. 3 frank
// .. 7, reached from the click lambdas and the drawer's update (0x2805A0). Its
// talk is the office's own, not a DialogController, and it is no PlayCutscene:
// on 2026-11-04 the host sat in Frank's and the client never left the House.
// The host's visit goes out as a start named "office:<npc>"; the client makes
// the same call on its own drawer, caught from the drawer's update (vtable
// 0xEE65A0 slot 10). NPCOffice (vtable 0xF034C0) updates at slot 10, which says
// one is up; its Exit button is init's lambda_1, _Do_call 0x285EB0 on a closure
// holding the office at +8 -- called on the client when the host's office closes.
constexpr uintptr_t kRva_GoToOffice     = 0x27CE70;
const uint8_t kOfficePrologue[9] = { 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08 };
constexpr uintptr_t kRva_DrawerVtable   = 0xEE65A0;
constexpr uintptr_t kRva_DrawerUpdate   = 0x277A30;
constexpr uintptr_t kRva_OfficeVtable   = 0xF034C0;
constexpr uintptr_t kRva_OfficeUpdate   = 0x284C30;
constexpr uintptr_t kRva_OfficeExitCall = 0x285EB0;
constexpr uintptr_t kRva_OfficeExitVt  = 0xF03D68;   // that lambda's _Func_impl, _Do_call at slot 2
constexpr uint64_t  kOfficeGoneMs       = 1500;       // its update pauses now and then
// THE INPUT GATE. The game's input update (0x986800) reads the mouse only when
// SDL_GetWindowFlags says the window has SDL_WINDOW_INPUT_FOCUS (0x200), at
// 0x986966 -- a second player's window in the background drops every click
// the host sends it. SDL_GetWindowFlags is exported; called from there, while
// the host's cutscene is up here, it says focused.
constexpr uintptr_t kRva_FocusCallRet   = 0x98696B;
constexpr uint64_t  kSdlInputFocus      = 0x200;
// WHAT THE NPC SAYS. An office asks its progression (global +0x5A8) which talk
// comes next: sub_1401C4410(progression, std::string* out, int npc), which
// constructs the talk's key in *out ("beanies_quest_fail"...), empty when there
// is none. Each game asks its own, and the client's House is the save before
// the host's pipe -- on 2026-11-04 Frank told the two players different things.
// The host's answers, asked from an office (init 0x280CE6 / 0x281C55, update
// 0x284D53), go out in order; the client's same calls get them instead. The
// drawer's own call (0x277A30, the map's alert icons) is left alone.
constexpr uintptr_t kRva_NextTalk = 0x1C4410;
const uint8_t kNextTalkPrologue[10] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10 };
const uintptr_t kTalkRets[] = { 0x280CE6, 0x281C55, 0x284D53 };
constexpr uint32_t kMaxTalks = 16;
const char kOfficePrefix[] = "office:";
const char kOfficeExit[]   = "office-exit";

using fn_office = uint64_t (__fastcall*)(void*, int);
using fn_exit   = void (__fastcall*)(void*);
using fn_flags  = uint64_t (__cdecl*)(void*);
using fn_talk   = void* (__fastcall*)(void*, void*, int);

using fn_play  = void* (__fastcall*)(void*, void*, void*, uint64_t, uint64_t);
using fn_alloc = void* (__fastcall*)(size_t);

struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size;
    uint64_t cap;
};

struct State {
    fn_update o_swf    = nullptr;
    fn_update o_dialog = nullptr;
    uint64_t  swf_at    = 0;
    uint64_t  dialog_at = 0;
    bool      was_up    = false;
    uint64_t  said_up_at = 0;   // host: when it last said so
    uint64_t  host_up_at = 0;   // client: when the host last said it was in one
    uint32_t  sent = 0, played = 0, swallowed = 0;

    // client: the host's inputs not posted yet
    struct In { uint8_t what; uint32_t pos; uint64_t at; };
    In        queue[16] = {};
    uint32_t  queued    = 0;

    fn_play   o_play    = nullptr;
    fn_alloc  str_alloc = nullptr;
    bool      playing_ours = false;
    // client: the last cutscene its own game started, and the host's not played
    char      own_name[48] = {};
    uint64_t  own_at       = 0;
    struct Start { char name[48]; uint32_t flags; uint64_t at; uint64_t first; bool held; };
    Start     starts[4] = {};
    uint32_t  nstarts   = 0;
    char      host_name[48] = {};   // the host's last start, kept for the skip
    uint64_t  host_at       = 0;
    void*     skip_seen     = nullptr;
    uint64_t  skip_since    = 0;
    bool      skipping      = false;
    char      skip_name[48] = {};
    char      cur_name[48]  = {};   // the last cutscene this game started, either role
    uint64_t  cur_at        = 0;
    char      host_up_name[48] = {};  // client: what the host says it is in
    char      said_id[48]   = {};     // host: what it last said
    uint64_t  host_input_at = 0;      // client: the host's last click or key
    uint64_t  blocked_since = 0;      // client: first swallowed click with none since
    bool      said_unblock  = false;
    uint64_t  host_dialog_down_at = 0;   // client: when the host's dialog last closed

    fn_update o_drawer = nullptr;
    fn_update o_office = nullptr;
    fn_office o_goto   = nullptr;
    fn_exit   office_exit = nullptr;
    void*     drawer      = nullptr;   // the House map's NPCMapDrawer, last seen
    uint64_t  drawer_at   = 0;
    void*     office      = nullptr;   // the NPCOffice up, last seen
    uint64_t  office_at   = 0;
    bool      office_was  = false;     // host: an office was up last frame
    bool      going_ours  = false;     // client: the host's visit, being made here
    bool      exit_wanted = false;     // client: the host left its office
    uint64_t  exit_at     = 0;
    int       office_go   = -1;        // client: the host's office, for the drawer's next update
    fn_exit   o_exit_btn  = nullptr;   // the office's Exit, hooked on its vtable
    fn_flags  o_flags     = nullptr;
    uintptr_t focus_ret   = 0;
    bool      said_focus  = false;

    fn_talk   o_talk      = nullptr;
    uintptr_t talk_rets[3] = {};
    void*     prog        = nullptr;    // the progression the game last asked
    uint32_t  talks_sent  = 0;          // host: this office's talks so far
    bool      talk_none   = false;      // host: the last one sent was "nothing"
    uint32_t  talks_used  = 0;          // client: this office's talks taken
    uint32_t  talks_have  = 0;          // client: the host's, arrived
    char      talks[kMaxTalks][48] = {};
};

State g;

bool host_up();

bool is_second_player() { return _stricmp(config().net_role, "client") == 0; }

void maybe_skip(void* swf) {
    const uint64_t now = GetTickCount64();
    if (swf != g.skip_seen) {
        g.skip_seen  = swf;
        g.skip_since = now;
        g.skipping   = false;
        if (g.cur_at && now - g.cur_at < 2000) strncpy_s(g.skip_name, g.cur_name, _TRUNCATE);
        else strncpy_s(g.skip_name, "?", _TRUNCATE);
    }
    if (!g.skipping) {
        bool always = false;
        for (const char* n : kAlwaysSkip) always |= strcmp(n, g.skip_name) == 0;
        if (!always) {
            if (now - g.skip_since < kSkipAfterMs) return;
            // The host watching THIS one -- by name, not just "a cutscene".
            const bool hosts =
                (host_up() && strcmp(g.host_up_name, g.skip_name) == 0) ||
                (g.host_at && now - g.host_at < kHostStartMs &&
                 strcmp(g.host_name, g.skip_name) == 0);
            if (hosts) { g.skip_since = now; return; }   // looked at again later
        }
        g.skipping = true;
        log_line("CUTSCENE", "skipping cutscene '%s' -- the host is not watching it",
                 g.skip_name);
    }
    const double done = 1.0;
    mem_write((uint8_t*)swf + kSwf_SkipProgress, &done, sizeof(done));
}

void* __fastcall swf_update(void* a, void* b, void* c, void* d) {
    const uint64_t now = GetTickCount64();
    // A cutscene that stopped updating is over, and the next one can be built
    // at the same address: on 2026-11-09 'king_intro' took the place of the
    // 'game_intro' skipped 40 minutes before and was skipped with it, silently.
    if (g.swf_at && now - g.swf_at > 1000) g.skip_seen = nullptr;
    g.swf_at = now;
    if (is_second_player()) maybe_skip(a);
    return g.o_swf(a, b, c, d);
}

// The controller updates every frame whether or not a box is up; its update
// does the work only with +0x1A8 set and a text at +0x48 (0x2D70A0). Counting
// every frame had the client's clicks swallowed for good on 2026-11-04.
constexpr uintptr_t kDialog_Active = 0x1A8;
constexpr uintptr_t kDialog_Text   = 0x48;

void* __fastcall dialog_update(void* a, void* b, void* c, void* d) {
    uint8_t active = 0;
    void*   text   = nullptr;
    if (mem_read((const uint8_t*)a + kDialog_Active, &active, 1) && active &&
        mem_read((const uint8_t*)a + kDialog_Text, &text, sizeof(text)) && text)
        g.dialog_at = GetTickCount64();
    return g.o_dialog(a, b, c, d);
}

bool office_up() { return g.o_office && GetTickCount64() - g.office_at < kOfficeGoneMs; }
bool safe_go_office(void* drawer, int npc);
bool safe_exit_office(void* office);

// Both calls are made from inside the game's own update of that component: the
// two reach the scene being updated through a global (0x13C18F0) the director
// sets only while it updates one, and from anywhere else it is null -- the
// first try, from the frame's end, faulted at 0x27D9F3 on 2026-11-04.
void* __fastcall drawer_update(void* a, void* b, void* c, void* d) {
    g.drawer    = a;
    g.drawer_at = GetTickCount64();
    void* r = g.o_drawer(a, b, c, d);
    startrun_house_tick();
    if (g.office_go >= 0) {
        const int npc = g.office_go;
        g.office_go  = -1;
        g.going_ours = true;
        const bool ok = safe_go_office(a, npc);
        g.going_ours = false;
        log_line("CUTSCENE", ok ? "<- the host's NPC office %d opened here"
                                : "!! the host's NPC office %d faulted opening here", npc);
    }
    return r;
}

void send_quest_items(bool only_changed = false);

void* __fastcall office_update(void* a, void* b, void* c, void* d) {
    g.office    = a;
    g.office_at = GetTickCount64();
    void* r = g.o_office(a, b, c, d);
    if (net_role() == NetRole::Host && net_active()) send_quest_items(true);
    if (g.exit_wanted && net_role() != NetRole::Host) {
        g.exit_wanted = false;
        const bool ok = safe_exit_office(a);
        log_line("CUTSCENE", ok ? "<- the host left its NPC office -- leaving this one"
                                : "!! leaving the NPC office faulted");
    }
    return r;
}

struct GameStr;
bool set_game_str(void* s, const char* text);

// THE QUEST ITEMS. MewDirector+0x5A8 is the GlobalProgressionData, and its
// +0x6C0 a std::vector<std::string> of quest items (begin, end, capacity end):
// an NPC office's "gather_questitem_info newest" (sub_1408EEA60) takes the last
// one, end - 0x20, unchecked. The friend's list was empty where the host's was
// not, and its game read before the list's start (2026-11-07, at 0x8F0B35).
constexpr uintptr_t kDir_Progress  = 0x5A8;
constexpr uintptr_t kProg_Quests   = 0x6C0;
constexpr uint32_t  kMaxQuests     = 32;
constexpr size_t    kStrSize       = 0x20;

uint8_t* quest_vector() {
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir  = nullptr;
    void* prog = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_Progress, &prog, sizeof(prog)) || !prog)
        return nullptr;
    return (uint8_t*)prog + kProg_Quests;
}

// THE NPC'S REWARD FLAG. The talk picker (sub_1401C4410) and "get npc_reward"
// (CacheInReward, sub_1401C18E0) both read GlobalProgressionData + npc * 0x78
// + 0x90: a reward is due. The host's was set by what it did in its House since
// the save the client loaded; the client's was not, so its office had nothing
// to say, and forced into the host's talk its "get npc_reward" handed out no
// quest item and "gather_questitem_info newest" went down (2026-11-07,
// Beanies). So each talk carries the host's flag, set here before this game
// picks: its own picker and its own reward then do what the host's did.
constexpr uintptr_t kProg_NpcStride = 0x78;
constexpr uintptr_t kProg_NpcReward = 0x90;
constexpr int       kMaxNpc         = 16;

uint8_t* npc_reward_flag(int npc) {
    if (npc < 0 || npc >= kMaxNpc) return nullptr;
    uint8_t* v = quest_vector();
    return v ? v - kProg_Quests + kProg_NpcReward + (uintptr_t)npc * kProg_NpcStride : nullptr;
}
uint32_t host_reward_aux(int npc) {
    uint8_t* f = npc_reward_flag(npc);
    uint8_t  b = 0;
    return f && mem_read(f, &b, 1) ? 0x100u | b : 0;
}

// HOST: its quest items, for the client's office to read the same. The list
// also grows in the middle of a talk (Beanies hands out the quest, then reads
// it back a few lines on), so the office's update sends it again whenever it
// changed (`only_changed`).
uint64_t g_quest_sent = 0;
void send_quest_items(bool only_changed) {
    if (net_role() != NetRole::Host || net_peer_count() < 2) return;
    uint8_t* v = quest_vector();
    uint8_t* q[3] = {};
    if (!v || !mem_read(v, q, sizeof(q))) return;
    uint32_t n = (q[0] && q[1] >= q[0]) ? (uint32_t)((q[1] - q[0]) / kStrSize) : 0;
    if (n > kMaxQuests) n = kMaxQuests;
    static char names[kMaxQuests][48];
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    for (uint32_t i = 0; i < n; ++i) {
        names[i][0] = 0;
        mem_read_std_string(q[0] + i * kStrSize, names[i], sizeof(names[i]));
        for (const char* s = names[i]; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
        h = (h ^ 0xFF) * 0x100000001B3ull;
    }
    if (only_changed && h == g_quest_sent) return;
    g_quest_sent = h;
    if (!n) {
        ChoiceMsg c{};
        c.kind = kChoiceQuestItem;
        net_send_choice(c);
    }
    for (uint32_t i = 0; i < n; ++i) {
        ChoiceMsg c{};
        c.kind  = kChoiceQuestItem;
        c.index = i;
        c.count = n;
        strncpy_s(c.name, names[i], _TRUNCATE);
        net_send_choice(c);
    }
    log_line("CUTSCENE", "-> the quest items (%u)%s%s", n, n ? ", newest " : "", n ? names[n - 1] : "");
}

bool make_name(GameStr& s, const char* text);
bool safe_alloc(size_t n, void** out);

// CLIENT: the host's quest items, written over this game's list.
char     g_quest[kMaxQuests][48];
uint32_t g_quest_n = 0;

void apply_quest_items(uint32_t n) {
    uint8_t* v = quest_vector();
    uint8_t* q[3] = {};
    if (!v || !mem_read(v, q, sizeof(q))) return;
    const uint32_t have = (q[0] && q[1] >= q[0]) ? (uint32_t)((q[1] - q[0]) / kStrSize) : 0;
    const uint32_t room = (q[0] && q[2] >= q[0]) ? (uint32_t)((q[2] - q[0]) / kStrSize) : 0;
    bool same = have == n;
    for (uint32_t i = 0; same && i < n; ++i) {
        char k[48] = {};
        mem_read_std_string(q[0] + i * kStrSize, k, sizeof(k));
        same = strcmp(k, g_quest[i]) == 0;
    }
    if (same) {
        log_line("CUTSCENE", "<- the host's quest items (%u) -- this game has the same", n);
        return;
    }
    uint8_t* base = q[0];
    uint32_t live = have;   // constructed strings at base
    if (n > room) {
        // A bigger buffer from the game's own new; the strings move bitwise
        // (MSVC's hold no pointer into themselves). The old buffer is left.
        void* p = nullptr;
        if (!safe_alloc((size_t)n * kStrSize, &p) || !p) {
            log_line("CUTSCENE", "!! could not make room for the host's %u quest item(s)", n);
            return;
        }
        if (have) memcpy(p, q[0], (size_t)have * kStrSize);
        base = (uint8_t*)p;
        q[2] = base + (size_t)n * kStrSize;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* s = base + i * kStrSize;
        if (i < live) set_game_str(s, g_quest[i]);
        else {
            GameStr fresh;
            if (!make_name(fresh, g_quest[i])) { n = i; break; }
            memcpy(s, &fresh, sizeof(fresh));
        }
    }
    // Strings past the new end are left as they are (a few bytes).
    q[0] = base;
    q[1] = base + (size_t)n * kStrSize;
    if (!mem_write(v, q, sizeof(q))) return;
    log_line("CUTSCENE", "<- the host's quest items (%u, this game had %u)%s%s", n, have,
             n ? ", newest " : "", n ? g_quest[n - 1] : "");
}

// See kRva_NextTalk.
uint32_t g_talk_aux[16] = {};

void* __fastcall next_talk(void* prog, void* out, int npc) {
    if (net_role() != NetRole::Host && net_active() && g.talks_used < g.talks_have &&
        g.talks_used < 16 && (g_talk_aux[g.talks_used] & 0x100)) {
        uint8_t* f = npc_reward_flag(npc);
        uint8_t  want = (uint8_t)(g_talk_aux[g.talks_used] & 0xFF), have = 0;
        if (f && mem_read(f, &have, 1) && have != want && mem_write(f, &want, 1))
            log_line("CUTSCENE", "<- NPC %d's reward flag %u, as the host's (this game had %u)",
                     npc, want, have);
    }
    void* r = g.o_talk(prog, out, npc);
    g.prog = prog;
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    bool from_office = false;
    for (uintptr_t a : g.talk_rets) from_office |= ret == a;
    if (!from_office || !net_active()) return r;
    char key[48];
    if (!mem_read_std_string(out, key, sizeof(key))) key[0] = 0;
    // An office with nothing left to say asks again every frame (2026-11-04:
    // eighty of them in two seconds). Only the first "nothing" counts.
    if (net_role() == NetRole::Host) {
        if (!key[0] && g.talk_none) return r;
        g.talk_none = !key[0];
        if (net_peer_count() >= 2) {
            send_quest_items();
            ChoiceMsg c{};
            c.kind  = kChoiceOfficeTalk;
            c.index = g.talks_sent++;
            c.aux   = host_reward_aux(npc);
            strncpy_s(c.name, key, _TRUNCATE);
            net_send_choice(c);
            log_line("CUTSCENE", "-> the NPC office's talk %u is '%s'", c.index, key);
        }
        return r;
    }
    const uint32_t i = g.talks_used;
    if (i < g.talks_have && i < kMaxTalks && !g.talks[i][0]) {
        // The host's said nothing more: so does this one, and it stays there.
        if (key[0]) set_game_str(out, "");
        return r;
    }
    if (i >= g.talks_have && !key[0]) return r;   // nothing here either, not a talk
    g.talks_used = i + 1;
    if (i >= g.talks_have || i >= kMaxTalks) {
        log_line("CUTSCENE", "!! the NPC office's talk %u is not in from the host -- this game's own '%s'",
                 i, key);
        return r;
    }
    if (strcmp(key, g.talks[i]) != 0) {
        if (set_game_str(out, g.talks[i]))
            log_line("CUTSCENE", "<- the NPC office's talk %u: the host's '%s', not '%s'", i,
                     g.talks[i], key);
        else
            log_line("CUTSCENE", "!! could not hand the office the host's talk '%s'", g.talks[i]);
    }
    return r;
}

// The office asks for its first talk seconds after it opens, and the client's
// asks at the same moment -- before the host's had crossed (2026-11-05: the
// friend's office talked as if no cat had gone down the pipe). So the host asks
// its own progression at once, as the map's icons do every frame, and sends
// that as talk 0 right behind the opening. The office's own ask sends 0 again.
void send_first_talk(int npc) {
    if (!g.o_talk || !g.prog) return;
    alignas(8) uint8_t tmp[sizeof(GameStr)] = {};
    char key[48] = {};
    __try {
        g.o_talk(g.prog, tmp, npc);
        if (!mem_read_std_string(tmp, key, sizeof(key))) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // A key past 15 characters is on the game's heap: left there, a few bytes.
    ChoiceMsg c{};
    c.kind  = kChoiceOfficeTalk;
    c.index = 0;
    c.aux   = host_reward_aux(npc);
    strncpy_s(c.name, key, _TRUNCATE);
    net_send_choice(c);
    log_line("CUTSCENE", "-> the NPC office's talk 0 is '%s' (asked at the opening)", key);
}

uint64_t __fastcall go_office(void* drawer, int npc) {
    if (net_role() == NetRole::Host) { g.talks_sent = 0; g.talk_none = false; }
    else if (g.going_ours) g.talks_used = 0;
    if (!g.going_ours && net_active()) {
        char n[48];
        _snprintf_s(n, sizeof(n), _TRUNCATE, "%s%d", kOfficePrefix, npc);
        if (net_role() == NetRole::Host) {
            if (net_peer_count() >= 2) {
                ChoiceMsg c{};
                c.kind = kChoiceCutStart;
                strncpy_s(c.name, n, _TRUNCATE);
                net_send_choice(c);
                log_line("CUTSCENE", "-> NPC office %d opened here, sent", npc);
                send_quest_items();
                send_first_talk(npc);
            }
        } else {
            strncpy_s(g.own_name, n, _TRUNCATE);
            g.own_at = GetTickCount64();
            log_line("CUTSCENE", "NPC office %d opened by this game", npc);
        }
    }
    return g.o_goto(drawer, npc);
}

bool safe_go_office(void* drawer, int npc) {
    __try { g.o_goto(drawer, npc); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The host pressing its office's Exit -- what closes the client's, not the
// office's update pausing, which it does: on 2026-11-04 a pause read as a close
// and the client was put back in its House with the host still talking.
bool linked();
void __fastcall exit_button(void* closure) {
    if (net_role() == NetRole::Host && linked() && !g.going_ours) {
        ChoiceMsg c{};
        c.kind = kChoiceCutStart;
        strncpy_s(c.name, kOfficeExit, _TRUNCATE);
        net_send_choice(c);
        log_line("CUTSCENE", "-> the NPC office's Exit was pressed here, sent");
    }
    g.o_exit_btn(closure);
}

bool focus_wanted();

uint64_t __cdecl window_flags(void* window) {
    uint64_t f = g.o_flags(window);
    if (!(f & kSdlInputFocus) && (uintptr_t)_ReturnAddress() == g.focus_ret && focus_wanted()) {
        if (!g.said_focus)
            log_line("CUTSCENE", "this window is in the background -- reading the host's clicks anyway");
        g.said_focus = true;
        f |= kSdlInputFocus;
    }
    return f;
}

bool safe_exit_office(void* office) {
    // The button's closure: its vtable is never read by _Do_call, the office is at +8.
    void* closure[2] = { nullptr, office };
    __try { g.office_exit(closure); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void patch(uintptr_t base, uintptr_t vt, uintptr_t want, void* ours, fn_update& orig,
           const char* what, uint32_t index = kUpdateSlot) {
    void** slot = (void**)(base + vt) + index;
    void*  have = nullptr;
    DWORD  prot = 0;
    if (!mem_read(slot, &have, sizeof(have)) || (uintptr_t)have != base + want ||
        !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
        log_line("CUTSCENE", "!! %s moved -- its cutscenes advance on each screen alone", what);
        return;
    }
    orig  = (fn_update)have;
    *slot = ours;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
}

// What says a cutscene is up. A dialog box only outside a battle: cats talk in
// battles too, and a battle's clicks are the battle's.
const char* up_why() {
    const uint64_t now = GetTickCount64();
    // Not the "Cutscene" scene: it is loaded from boot on, cutscene or not.
    if (g.o_swf && now - g.swf_at < kGoneMs) return "a swf cutscene";
    if (office_up()) return "an NPC office";
    if (g.o_dialog && now - g.dialog_at < kGoneMs && !lockstep_in_battle()) return "a dialog";
    return nullptr;
}

bool linked() { return net_active() && net_peer_count() >= 2; }

// Client: a cutscene of the host's is up here, so its clicks must land even
// with this window behind another. The client's own are swallowed meanwhile.
bool same_as_host();
bool dialog_is_hosts();
bool focus_wanted() {
    return net_role() != NetRole::Host && linked() && up_why() != nullptr &&
           (same_as_host() || dialog_is_hosts() || g.queued > 0);
}

// A client alone in a dialog -- an NPC in its own House -- answers it itself.
bool host_up() { return GetTickCount64() - g.host_up_at < kHostUpForMs; }

// Which cutscene this game is in, comparable across the two: a swf one by the
// name it was started under, the rest by kind.
const char* my_id() {
    const uint64_t now = GetTickCount64();
    if (g.o_swf && now - g.swf_at < kGoneMs) return g.cur_name[0] ? g.cur_name : "swf";
    if (office_up()) return "office";
    if (g.o_dialog && now - g.dialog_at < kGoneMs && !lockstep_in_battle()) return "dialog";
    return "";
}

// The host's clicks go into the same cutscene only.
bool same_as_host() { return host_up() && strcmp(my_id(), g.host_up_name) == 0; }

// A dialog box closes for a moment between two of an NPC's boxes, and the host
// may be a reload ahead or behind: a dialog here is the host's to click through
// while the host is in one or left one this recently. Past that the host is
// elsewhere -- an NPC the client opened alone -- and the clicks are the client's.
constexpr uint64_t kHostAwayMs = 6000;
bool dialog_is_hosts() {
    const char* id = my_id();
    if (strcmp(id, "dialog") != 0 && strcmp(id, "office") != 0) return false;
    if (host_up() && strcmp(g.host_up_name, id) == 0) return true;
    return g.host_dialog_down_at && GetTickCount64() - g.host_dialog_down_at < kHostAwayMs;
}

void say(uint32_t state, const char* name = nullptr) {
    ChoiceMsg c{};
    c.kind = kChoiceCutInput;
    c.aux  = state;
    if (name) strncpy_s(c.name, name, _TRUNCATE);
    net_send_choice(c);
}

void read_name(const void* s, char* out, size_t cap) {
    out[0] = 0;
    if (!mem_read_std_string(s, out, cap)) out[0] = 0;
}

void* __fastcall play(void* name, void* on_start, void* on_end, uint64_t a, uint64_t b) {
    char n[48];
    read_name(name, n, sizeof(n));
    if (n[0]) {
        strncpy_s(g.cur_name, n, _TRUNCATE);
        g.cur_at = GetTickCount64();
    }
    if (!g.playing_ours && n[0] && net_active()) {
        if (net_role() == NetRole::Host) {
            if (net_peer_count() >= 2) {
                ChoiceMsg c{};
                c.kind = kChoiceCutStart;
                c.aux  = (uint32_t)((a & 0xFF) | ((b & 0xFF) << 8));
                strncpy_s(c.name, n, _TRUNCATE);
                net_send_choice(c);
                log_line("CUTSCENE", "-> cutscene '%s' started here, sent", n);
            }
        } else {
            strncpy_s(g.own_name, n, _TRUNCATE);
            g.own_at = GetTickCount64();
            log_line("CUTSCENE", "cutscene '%s' started by this game", n);
        }
    }
    return g.o_play(name, on_start, on_end, a, b);
}

bool safe_alloc(size_t n, void** out) {
    __try { *out = g.str_alloc(n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool make_name(GameStr& s, const char* text) {
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) {
        memcpy(s.buf, text, n + 1);
        s.size = n;
        s.cap  = 15;
        return true;
    }
    size_t cap = n | 15;
    if (cap < 22) cap = 22;
    void* p = nullptr;
    if (!g.str_alloc || !safe_alloc(cap + 1, &p) || !p) return false;
    memcpy(p, text, n + 1);
    s.ptr  = (char*)p;
    s.size = n;
    s.cap  = cap;
    return true;
}

// Overwrites a constructed std::string the game owns. Its old buffer is kept
// when the text fits; otherwise a new one comes from the game's heap and the
// old one is left behind (a few bytes, once a talk).
bool set_game_str(void* p, const char* text) {
    GameStr& s = *(GameStr*)p;
    const size_t n = strlen(text);
    if (n <= s.cap) {
        char* buf = s.cap > 15 ? s.ptr : s.buf;
        memcpy(buf, text, n + 1);
        s.size = n;
        return true;
    }
    GameStr fresh;
    if (!make_name(fresh, text)) return false;
    s = fresh;
    return true;
}

bool safe_play(GameStr* name, void* f1, void* f2, uint64_t a, uint64_t b) {
    __try { g.o_play(name, f1, f2, a, b); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Client: the host's cutscene, here. Two empty std::functions, all zero being
// what an empty one is; the name on the game's heap, since the game frees it.
void play_hosts(const char* name, uint32_t flags) {
    if (strncmp(name, kOfficePrefix, sizeof(kOfficePrefix) - 1) == 0) {
        g.office_go = atoi(name + sizeof(kOfficePrefix) - 1);   // see drawer_update
        return;
    }
    GameStr s;
    alignas(16) uint8_t f1[0x40] = {};
    alignas(16) uint8_t f2[0x40] = {};
    if (!make_name(s, name)) {
        log_line("CUTSCENE", "!! could not hand the game the name '%s'", name);
        return;
    }
    g.playing_ours = true;
    const bool ok = safe_play(&s, f1, f2, flags & 0xFF, (flags >> 8) & 0xFF);
    g.playing_ours = false;
    log_line("CUTSCENE", ok ? "<- the host's cutscene '%s' started here"
                            : "!! the host's cutscene '%s' faulted starting here", name);
}

// Client, once a frame: play what the host played and this game did not.
void start_pump(bool up) {
    const uint64_t now = GetTickCount64();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g.nstarts; ++i) {
        State::Start& s = g.starts[i];
        const bool own = g.own_at && strcmp(g.own_name, s.name) == 0 &&
                         (g.own_at + kOwnWindowMs >= s.first);
        if (own) {
            log_line("CUTSCENE", "the host's cutscene '%s' started here by itself", s.name);
            continue;
        }
        // Still loading the House the host's night sent (2026-11-07: the
        // guillotine's cutscene came 10 s into the friend's reload and never
        // played): it waits, and its time counts from the load's end -- which
        // also gives this game's own start of it its chance first.
        const bool office0 = strncmp(s.name, kOfficePrefix, sizeof(kOfficePrefix) - 1) == 0;
        if (!office0 && (leave_loading(nullptr) || leave_busy())) {
            if (!s.held)
                log_line("CUTSCENE", "the host's cutscene '%s' waits for this game's loading", s.name);
            s.held = true;
            s.at   = now;
            g.starts[kept++] = s;
            continue;
        }
        if (now - s.at > kStartStaleMs) {
            log_line("CUTSCENE", "!! the host's cutscene '%s' never got its turn here", s.name);
            continue;
        }
        // An office: from the House map only, so not before this game's drawer
        // is there -- a client still reloading the House gets it once loaded.
        const bool office = strncmp(s.name, kOfficePrefix, sizeof(kOfficePrefix) - 1) == 0;
        if (office && g.exit_wanted) {
            log_line("CUTSCENE", "the host's NPC office closed before it opened here");
            continue;
        }
        const bool ready = office
            ? (g.o_goto && g.drawer && now - g.drawer_at < kGoneMs && now - s.at >= 300)
            : now - s.at >= kOwnWindowMs;
        // Not on top of another, not on the way to the menu, and not before this
        // game has had its own chance to start it.
        if (kept == 0 && !up && !leave_busy() && ready) {
            play_hosts(s.name, s.flags);
            continue;
        }
        g.starts[kept++] = s;
    }
    g.nstarts = kept;
}

const char* input_name(uint8_t what) {
    return what == 0 ? "click" : what == 1 ? "space" : "enter";
}

} // namespace

// ---------------------------------------------------------------------------

void cutscene_set_base(uintptr_t base) {
    if (g.o_swf || g.o_dialog) return;
    {
        void* at = (void*)(base + kRva_GoToOffice);
        if (memcmp(at, kOfficePrologue, sizeof(kOfficePrologue)) != 0 ||
            MH_CreateHook(at, (void*)&go_office, (void**)&g.o_goto) != MH_OK ||
            MH_EnableHook(at) != MH_OK) {
            g.o_goto = nullptr;
            log_line("CUTSCENE", "!! GoToNPCOffice moved -- NPC offices open on one screen only");
        }
        g.office_exit = (fn_exit)(base + kRva_OfficeExitCall);
        for (int i = 0; i < 3; ++i) g.talk_rets[i] = base + kTalkRets[i];
        void* nt = (void*)(base + kRva_NextTalk);
        if (memcmp(nt, kNextTalkPrologue, sizeof(kNextTalkPrologue)) != 0 ||
            MH_CreateHook(nt, (void*)&next_talk, (void**)&g.o_talk) != MH_OK ||
            MH_EnableHook(nt) != MH_OK) {
            g.o_talk = nullptr;
            log_line("CUTSCENE", "!! the NPC talk picker moved -- each game's NPCs say their own");
        }
        patch(base, kRva_DrawerVtable, kRva_DrawerUpdate, (void*)&drawer_update, g.o_drawer,
              "NPCMapDrawer");
        patch(base, kRva_OfficeVtable, kRva_OfficeUpdate, (void*)&office_update, g.o_office,
              "NPCOffice");
        {
            fn_update tmp = nullptr;
            patch(base, kRva_OfficeExitVt, kRva_OfficeExitCall, (void*)&exit_button, tmp,
                  "the NPC office's Exit", 2);
            g.o_exit_btn = (fn_exit)tmp;
        }
        void* gf = (void*)GetProcAddress(GetModuleHandleW(nullptr), "SDL_GetWindowFlags");
        g.focus_ret = base + kRva_FocusCallRet;
        // The call there must be to it, or the gate is somewhere else now.
        int32_t rel = 0;
        const bool there = mem_read((const uint8_t*)base + kRva_FocusCallRet - 4, &rel, 4) &&
                           gf && (uintptr_t)base + kRva_FocusCallRet + rel == (uintptr_t)gf;
        if (!there || MH_CreateHook(gf, (void*)&window_flags, (void**)&g.o_flags) != MH_OK ||
            MH_EnableHook(gf) != MH_OK) {
            g.o_flags = nullptr;
            log_line("CUTSCENE", "!! the input focus check moved -- a background window misses the host's clicks");
        }
    }
    g.str_alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    void* at = (void*)(base + kRva_PlayCutscene);
    if (memcmp(at, kPlayPrologue, sizeof(kPlayPrologue)) != 0 ||
        MH_CreateHook(at, (void*)&play, (void**)&g.o_play) != MH_OK ||
        MH_EnableHook(at) != MH_OK) {
        g.o_play = nullptr;
        log_line("CUTSCENE", "!! the cutscene start moved -- each game starts its own");
    }
    patch(base, kRva_SwfVtable, kRva_SwfUpdate, (void*)&swf_update, g.o_swf, "SwfCutscene");
    patch(base, kRva_DialogVtable, kRva_DialogUpdate, (void*)&dialog_update, g.o_dialog,
          "DialogController");
}

void cutscene_pump() {
    const char* why = up_why();
    const bool  up  = why != nullptr;
    if (up != g.was_up) {
        if (up) log_line("CUTSCENE", "a cutscene is up (%s)%s", why,
                         !linked() ? "" : net_role() == NetRole::Host
                             ? " -- your clicks move it on both screens"
                             : " -- the host's clicks move it here");
        else    log_line("CUTSCENE", "the cutscene is over");
        g.was_up = up;
        if (!up && linked() && net_role() == NetRole::Host) {
            say(kHostDown);
            // The next box is news at once, not a second later.
            g.said_up_at = 0;
            g.said_id[0] = 0;
        }
    }
    // The host's Exit (exit_button) closes the client's office, in office_update.
    if (net_role() != NetRole::Host && g.exit_wanted) {
        // Left from office_update; this only gives up when none comes up here.
        const uint64_t now = GetTickCount64();
        if (!office_up() && g.office_go < 0 && now - g.exit_at > 4000) {
            g.exit_wanted = false;   // none came up here to leave
        }
    }
    if (net_role() != NetRole::Host && g.o_play) start_pump(up);
    if (net_role() == NetRole::Host) {
        const uint64_t now = GetTickCount64();
        const char* id = my_id();
        if (up && linked() &&
            (now - g.said_up_at >= kHostUpEveryMs || strcmp(id, g.said_id) != 0)) {
            g.said_up_at = now;
            strncpy_s(g.said_id, id, _TRUNCATE);
            say(kHostUp, id);
        }
        return;
    }

    // The host may be a frame or a node ahead: an input that came before this
    // peer's cutscene waits for it, and goes stale rather than land on the map.
    const uint64_t now = GetTickCount64();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g.queued; ++i) {
        if (now - g.queue[i].at > kStaleInputMs) continue;
        if (up && kept == 0 && i == 0 && same_as_host() && ui_post_input(g.queue[i].what, g.queue[i].pos)) {
            ++g.played;
            log_line("CUTSCENE", "<- the host's %s", input_name(g.queue[i].what));
            continue;
        }
        g.queue[kept++] = g.queue[i];
    }
    g.queued = kept;
}

bool cutscene_on_message(const ChoiceMsg& m) {
    if (m.kind == kChoiceQuestItem) {
        if (net_role() == NetRole::Host) return true;
        if (m.count == 0) { g_quest_n = 0; return true; }
        if (m.count > kMaxQuests || m.index >= m.count) return true;
        if (m.index == 0) g_quest_n = 0;
        if (m.index != g_quest_n) return true;   // one went missing: wait for the next list
        strncpy_s(g_quest[m.index], m.name, _TRUNCATE);
        g_quest_n = m.index + 1;
        if (g_quest_n == m.count)
            log_line("CUTSCENE", "<- the host has %u quest item(s), newest %s", m.count,
                     g_quest[m.count - 1]);
        return true;
    }
    if (m.kind == kChoiceOfficeTalk) {
        if (net_role() == NetRole::Host) return true;
        if (m.index < kMaxTalks) {
            if (m.index < 16) g_talk_aux[m.index] = m.aux;
            strncpy_s(g.talks[m.index], m.name, _TRUNCATE);
            if (m.index + 1 > g.talks_have) g.talks_have = m.index + 1;
        }
        return true;
    }
    if (m.kind == kChoiceCutStart) {
        if (net_role() == NetRole::Host || !m.name[0]) return true;
        if (strcmp(m.name, kOfficeExit) == 0) {
            g.exit_wanted = true;
            g.exit_at     = GetTickCount64();
            return true;
        }
        if (strncmp(m.name, kOfficePrefix, sizeof(kOfficePrefix) - 1) == 0) {
            g.exit_wanted = false;
            g.talks_have  = 0;   // this office's talks follow this
            g.talks_used  = 0;
        }
        if (g.nstarts < 4) {
            State::Start& s = g.starts[g.nstarts++];
            strncpy_s(s.name, m.name, _TRUNCATE);
            s.flags = m.aux;
            s.at    = GetTickCount64();
            s.first = s.at;
            s.held  = false;
        }
        strncpy_s(g.host_name, m.name, _TRUNCATE);
        g.host_at = GetTickCount64();
        return true;
    }
    if (m.kind != kChoiceCutInput) return false;
    if (net_role() == NetRole::Host) return true;
    if (m.aux == kHostUp) {
        g.host_up_at = GetTickCount64();
        strncpy_s(g.host_up_name, m.name, _TRUNCATE);
        return true;
    }
    if (m.aux == kHostDown) {
        if (host_up() && strcmp(g.host_up_name, "dialog") == 0)
            g.host_dialog_down_at = GetTickCount64();
        g.host_up_at = 0;
        return true;
    }
    g.host_up_at    = GetTickCount64();
    g.host_input_at = g.host_up_at;
    // count: where the host clicked (ui_pack_pos) -- an NPC office moves on
    // only for a click on its speech box, not for one under the client's cursor.
    if (g.queued < 16)
        g.queue[g.queued++] = { (uint8_t)(m.aux > 2 ? 0 : m.aux), m.aux == 0 ? m.count : 0u,
                                GetTickCount64() };
    return true;
}

bool cutscene_filter_input(unsigned msg, uintptr_t wparam, intptr_t lparam) {
    if (!linked()) return false;
    uint8_t what = 0;
    bool    down = false;
    switch (msg) {
        case WM_LBUTTONDOWN: what = 0; down = true; break;
        case WM_LBUTTONUP:   what = 0; break;
        case WM_KEYDOWN: case WM_KEYUP:
            if (wparam != VK_SPACE && wparam != VK_RETURN) return false;
            what = wparam == VK_SPACE ? 1 : 2;
            down = msg == WM_KEYDOWN;
            break;
        default: return false;
    }
    if (!up_why()) return false;

    if (net_role() == NetRole::Host) {
        if (down) {
            ChoiceMsg c{};
            c.kind = kChoiceCutInput;
            c.aux  = what;
            c.index = ++g.sent;
            if (what == 0) c.count = ui_pack_pos(lparam);
            net_send_choice(c);
        }
        return false;
    }
    const bool hosts_dialog = dialog_is_hosts();
    if (!same_as_host() && !hosts_dialog) return false;
    // Never for long without the host clicking: whatever this is, it is not
    // worth leaving the second player without a mouse. Not in a dialog the host
    // is in: the host reading a long one is not the host gone.
    if (!hosts_dialog) {
        constexpr uint64_t kUnblockMs = 8000;
        const uint64_t now = GetTickCount64();
        if (g.host_input_at && now - g.host_input_at < kUnblockMs) g.blocked_since = 0;
        else if (!g.blocked_since) g.blocked_since = now;
        if (g.blocked_since && now - g.blocked_since > kUnblockMs) {
            if (!g.said_unblock)
                log_line("CUTSCENE", "the host has not clicked for a while -- your clicks are yours again");
            g.said_unblock = true;
            return false;
        }
        g.said_unblock = false;
    }
    if (down && g.swallowed++ == 0)
        log_line("CUTSCENE", "your clicks in a cutscene wait for the host's");
    return true;
}

} // namespace mgmp
