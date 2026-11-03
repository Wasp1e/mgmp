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
    struct In { uint8_t what; uint64_t at; };
    In        queue[16] = {};
    uint32_t  queued    = 0;

    fn_play   o_play    = nullptr;
    fn_alloc  str_alloc = nullptr;
    bool      playing_ours = false;
    // client: the last cutscene its own game started, and the host's not played
    char      own_name[48] = {};
    uint64_t  own_at       = 0;
    struct Start { char name[48]; uint32_t flags; uint64_t at; };
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
    g.swf_at = GetTickCount64();
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

void patch(uintptr_t base, uintptr_t vt, uintptr_t want, void* ours, fn_update& orig,
           const char* what) {
    void** slot = (void**)(base + vt) + kUpdateSlot;
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
    if (g.o_dialog && now - g.dialog_at < kGoneMs && !lockstep_in_battle()) return "a dialog";
    return nullptr;
}

bool linked() { return net_active() && net_peer_count() >= 2; }

// A client alone in a dialog -- an NPC in its own House -- answers it itself.
bool host_up() { return GetTickCount64() - g.host_up_at < kHostUpForMs; }

// Which cutscene this game is in, comparable across the two: a swf one by the
// name it was started under, the rest by kind.
const char* my_id() {
    const uint64_t now = GetTickCount64();
    if (g.o_swf && now - g.swf_at < kGoneMs) return g.cur_name[0] ? g.cur_name : "swf";
    if (g.o_dialog && now - g.dialog_at < kGoneMs && !lockstep_in_battle()) return "dialog";
    return "";
}

// The host's clicks go into the same cutscene only.
bool same_as_host() { return host_up() && strcmp(my_id(), g.host_up_name) == 0; }

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

bool safe_play(GameStr* name, void* f1, void* f2, uint64_t a, uint64_t b) {
    __try { g.o_play(name, f1, f2, a, b); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Client: the host's cutscene, here. Two empty std::functions, all zero being
// what an empty one is; the name on the game's heap, since the game frees it.
void play_hosts(const char* name, uint32_t flags) {
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
                         (g.own_at + kOwnWindowMs >= s.at);
        if (own) {
            log_line("CUTSCENE", "the host's cutscene '%s' started here by itself", s.name);
            continue;
        }
        if (now - s.at > kStartStaleMs) {
            log_line("CUTSCENE", "!! the host's cutscene '%s' never got its turn here", s.name);
            continue;
        }
        // Not on top of another, not on the way to the menu, and not before this
        // game has had its own chance to start it.
        if (kept == 0 && !up && !leave_busy() && now - s.at >= kOwnWindowMs) {
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
        if (!up && linked() && net_role() == NetRole::Host) say(kHostDown);
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
        if (up && kept == 0 && i == 0 && same_as_host() && ui_post_input(g.queue[i].what)) {
            ++g.played;
            log_line("CUTSCENE", "<- the host's %s", input_name(g.queue[i].what));
            continue;
        }
        g.queue[kept++] = g.queue[i];
    }
    g.queued = kept;
}

bool cutscene_on_message(const ChoiceMsg& m) {
    if (m.kind == kChoiceCutStart) {
        if (net_role() == NetRole::Host || !m.name[0]) return true;
        if (g.nstarts < 4) {
            State::Start& s = g.starts[g.nstarts++];
            strncpy_s(s.name, m.name, _TRUNCATE);
            s.flags = m.aux;
            s.at    = GetTickCount64();
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
    if (m.aux == kHostDown) { g.host_up_at = 0; return true; }
    g.host_up_at    = GetTickCount64();
    g.host_input_at = g.host_up_at;
    if (g.queued < 16) g.queue[g.queued++] = { (uint8_t)(m.aux > 2 ? 0 : m.aux), GetTickCount64() };
    return true;
}

bool cutscene_filter_input(unsigned msg, uintptr_t wparam) {
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
            net_send_choice(c);
        }
        return false;
    }
    if (!same_as_host()) return false;
    // Never for long without the host clicking: whatever this is, it is not
    // worth leaving the second player without a mouse.
    {
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
