#include "mgmp_startrun.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"
#include "mgmp_leave.h"
#include "MinHook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// MewDirector::GoOnAnAdventure(std::string map, vector<string> collars,
// CustomVectorInterface<int64> cats), sub_1403B13F0: the box of cats confirmed
// in the House (sub_1400AE360, "alley.gon" and the collars the progression has
// unlocked, sub_1401C5CA0). It loads the ClassChooser; the gear, the map and the
// run's real start (sub_1403B01C0) come after. All three by value, so the
// callee destroys them: what the client hands it is the game's own memory --
// strings and the collar array from its operator new, the cats from its
// realloc (sub_140D4CB38), the way sub_1400483A0 copies such a list.
constexpr uintptr_t kRva_GoOnAdventure = 0x3B13F0;
const uint8_t kGoPrologue[8] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44 };
constexpr uintptr_t kRva_Realloc = 0xD4CB38;
const uint8_t kReallocThunk[1] = { 0xE9 };

// THE EMBARK CUTSCENE. sub_1403C0210(director-ish global 0x13DAC30, std::string
// kind, std::function done, double) plays a transition cutscene by kind;
// "embark" is the cats setting off (the gear screen's confirm, 0x34D997). Both
// by value, so the callee destroys them. The client's is played with a
// `done` of the mod's own that does nothing: the real one sets this game off on
// a run of its own, and the client's run is the host's, by its save.
constexpr uintptr_t kRva_PlayCutscene = 0x3C0210;
const uint8_t kPlayCutscenePrologue[8] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0xF2 };
constexpr uintptr_t kRva_CutsceneOwner = 0x13DAC30;

constexpr uint32_t kMaxParty   = 8;
constexpr uint32_t kMaxCollars = 16;
constexpr uint64_t kStaleMs    = 30000;   // a start not carried out by then is dropped

struct GameStr {
    union { char buf[16]; char* ptr; };
    uint64_t size;
    uint64_t cap;
};
struct StrVec { GameStr* begin; GameStr* end; GameStr* cap; };
struct KeyVec { uint32_t cap; uint32_t count; uint64_t* data; };

using fn_go      = void* (__fastcall*)(void*, void*, void*, void*);
using fn_play    = void* (__fastcall*)(void*, void*, void*, double);
using fn_alloc   = void* (__cdecl*)(size_t);
using fn_realloc = void* (__cdecl*)(void*, size_t);

struct State {
    uintptr_t  base_    = 0;
    fn_go      o_go     = nullptr;
    fn_play    o_play   = nullptr;
    bool       playing_ours = false;
    bool       cut_pending  = false;
    uint64_t   cut_at       = 0;
    char       cut_kind[48] = {};
    fn_alloc   alloc    = nullptr;
    fn_realloc realloc_ = nullptr;
    bool       ours     = false;    // the client's own call, from the host's

    // client: the host's start, waiting for a House update
    bool     pending    = false;
    uint64_t pending_at = 0;
    char     map[48]    = {};
    uint64_t party[kMaxParty] = {};
    uint32_t party_n    = 0;
    char     collars[kMaxCollars][48] = {};
    uint32_t collars_n  = 0;
};
State g;

void* director() {
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir))) return nullptr;
    return dir;
}

bool read_str(const void* s, char* out, size_t cap) {
    out[0] = 0;
    GameStr gs{};
    if (!mem_read(s, &gs, sizeof(gs)) || gs.size >= cap) return false;
    const void* data = gs.cap > 15 ? (const void*)gs.ptr : s;
    if (gs.size && !mem_read(data, out, (size_t)gs.size)) return false;
    out[gs.size] = 0;
    return true;
}

void send(uint8_t kind, uint32_t index, uint32_t count, uint64_t cat, const char* name) {
    ChoiceMsg m{};
    m.kind  = kind;
    m.index = index;
    m.count = count;
    m.cat   = cat;
    if (name) strncpy_s(m.name, name, _TRUNCATE);
    net_send_choice(m);
}

bool host_with_peer() {
    return net_active() && net_role() == NetRole::Host && net_peer_count() >= 2;
}

// HOST: the box of cats goes out as it is confirmed.
void* __fastcall go_on_adventure(void* dir, void* map, void* collars, void* cats) {
    if (!g.ours && host_with_peer()) {
        char name[48];
        read_str(map, name, sizeof(name));
        StrVec cv{};
        uint32_t nc = 0;
        if (collars && mem_read(collars, &cv, sizeof(cv)) && cv.begin && cv.end >= cv.begin)
            nc = (uint32_t)(cv.end - cv.begin);
        if (nc > kMaxCollars) nc = kMaxCollars;
        for (uint32_t i = 0; i < nc; ++i) {
            char cls[48];
            read_str(&cv.begin[i], cls, sizeof(cls));
            send(kChoiceRunCollar, i, nc, 0, cls);
        }
        KeyVec kv{};
        uint32_t nk = 0;
        if (cats && mem_read(cats, &kv, sizeof(kv)) && kv.data) nk = kv.count;
        if (nk > kMaxParty) nk = kMaxParty;
        for (uint32_t i = 0; i < nk; ++i) {
            uint64_t k = 0;
            mem_read(&kv.data[i], &k, sizeof(k));
            send(kChoiceRunParty, i, nk, k, nullptr);
        }
        send(kChoiceRunStart, 0, 0, 0, name);
        log_line("STARTRUN", "-> the host sets off: '%s', %u collar(s), %u cat(s) -- the other"
                             " player's game opens its own class chooser", name, nc, nk);
    }
    return g.o_go(dir, map, collars, cats);
}

bool safe_alloc(size_t n, void** out) {
    __try { *out = g.alloc(n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_realloc(size_t n, void** out) {
    __try { *out = g.realloc_(nullptr, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_go(void* dir, GameStr* map, StrVec* collars, KeyVec* cats) {
    __try { g.o_go(dir, map, collars, cats); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool make_str(GameStr& s, const char* text) {
    const size_t n = strlen(text);
    memset(&s, 0, sizeof(s));
    if (n <= 15) {
        memcpy(s.buf, text, n + 1);
        s.size = n;
        s.cap  = 15;
        return true;
    }
    void* p = nullptr;
    if (!safe_alloc(n + 1, &p) || !p) return false;
    memcpy(p, text, n + 1);
    s.ptr  = (char*)p;
    s.size = n;
    s.cap  = n;
    return true;
}

// CLIENT: the host's box of cats, confirmed in this game's House too.
void go_after_host() {
    void* dir = director();
    if (!dir || !g.party_n || !g.collars_n || !g.alloc || !g.realloc_) {
        log_line("STARTRUN", "!! the host's start lacks something here (director %p, %u cat(s),"
                             " %u collar(s)) -- not followed", dir, g.party_n, g.collars_n);
        return;
    }
    GameStr map{};
    void* arr = nullptr;
    void* keys = nullptr;
    if (!make_str(map, g.map) || !safe_alloc(g.collars_n * sizeof(GameStr), &arr) || !arr ||
        !safe_realloc(g.party_n * sizeof(uint64_t), &keys) || !keys) {
        log_line("STARTRUN", "!! could not build the host's start here");
        return;
    }
    GameStr* strs = (GameStr*)arr;
    for (uint32_t i = 0; i < g.collars_n; ++i)
        if (!make_str(strs[i], g.collars[i])) make_str(strs[i], "Colorless");
    StrVec collars{ strs, strs + g.collars_n, strs + g.collars_n };
    memcpy(keys, g.party, g.party_n * sizeof(uint64_t));
    KeyVec cats{ g.party_n, g.party_n, (uint64_t*)keys };
    g.ours = true;
    const bool ok = safe_go(dir, &map, &collars, &cats);
    g.ours = false;
    log_line("STARTRUN", ok ? "<- the host's box of cats ('%s', %u cat(s), %u collar(s)) set off"
                              " here too -- this game's class chooser opens"
                            : "!! following the host's box of cats faulted ('%s', %u cat(s), %u"
                              " collar(s))",
             g.map, g.party_n, g.collars_n);
}

// A std::function<void()> that does nothing, in MSVC's layout: 0x38 bytes of
// storage holding the callable, the _Func_base* at +0x38. The callable here is
// just a vftable pointer, small enough to live in the storage; its slots are
// _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get.
struct NoopFn;
void* __fastcall noop_copy(void* self, void* where);
void* __fastcall noop_move(void* self, void* where);
void  __fastcall noop_call(void* self) {
    (void)self;
    log_line("STARTRUN", "the embark cutscene is over here -- this game waits for the host's run");
}
const void* __fastcall noop_type(void* self) { (void)self; return nullptr; }
void  __fastcall noop_delete(void* self, bool dealloc) { (void)self; (void)dealloc; }
void* __fastcall noop_get(void* self) { return self; }
void* const kNoopVt[6] = { (void*)&noop_copy, (void*)&noop_move, (void*)&noop_call,
                           (void*)&noop_type, (void*)&noop_delete, (void*)&noop_get };
void* __fastcall noop_copy(void* self, void* where) { (void)self; *(const void**)where = kNoopVt; return where; }
void* __fastcall noop_move(void* self, void* where) { (void)self; *(const void**)where = kNoopVt; return where; }

struct GameFn { void* storage[7]; void* impl; };

// HOST: an embark cutscene goes out as it starts.
void* __fastcall play_cutscene(void* owner, void* kind, void* done, double t) {
    if (!g.playing_ours && host_with_peer()) {
        char name[48];
        read_str(kind, name, sizeof(name));
        if (strcmp(name, "embark") == 0) {
            send(kChoiceRunCutscene, 0, 0, 0, name);
            log_line("STARTRUN", "-> the host's '%s' cutscene, for the other player's screen", name);
        }
    }
    return g.o_play(owner, kind, done, t);
}

bool safe_play(void* owner, GameStr* kind, GameFn* done) {
    __try { g.o_play(owner, kind, done, 0.0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void startrun_scene_tick() {
    if (!g.cut_pending || !g.o_play) return;
    g.cut_pending = false;
    if (GetTickCount64() - g.cut_at > kStaleMs) return;
    void* owner = nullptr;
    void* const* slot = (void* const*)(g.base_ + kRva_CutsceneOwner);
    if (!mem_read(slot, &owner, sizeof(owner)) || !owner) {
        log_line("STARTRUN", "!! no cutscene owner here -- the host's cutscene is not played");
        return;
    }
    GameStr kind{};
    make_str(kind, g.cut_kind);
    GameFn done{};
    done.storage[0] = (void*)kNoopVt;
    done.impl       = &done.storage[0];
    g.playing_ours = true;
    const bool ok = safe_play(owner, &kind, &done);
    g.playing_ours = false;
    log_line("STARTRUN", ok ? "<- the host's '%s' cutscene plays here too"
                            : "!! the host's '%s' cutscene faulted here", g.cut_kind);
}

void startrun_set_base(uintptr_t base) {
    if (g.o_go) return;
    g.base_ = base;
    void* pc = (void*)(base + kRva_PlayCutscene);
    if (memcmp(pc, kPlayCutscenePrologue, 8) != 0 ||
        MH_CreateHook(pc, (void*)&play_cutscene, (void**)&g.o_play) != MH_OK ||
        MH_EnableHook(pc) != MH_OK) {
        g.o_play = nullptr;
        log_line("STARTRUN", "!! the cutscene player moved -- the other player does not see the embark");
    }
    g.alloc = (fn_alloc)addr_of_call(C_GameStrAlloc);
    if (memcmp((const void*)(base + kRva_Realloc), kReallocThunk, 1) == 0)
        g.realloc_ = (fn_realloc)(base + kRva_Realloc);
    void* at = (void*)(base + kRva_GoOnAdventure);
    if (memcmp(at, kGoPrologue, 8) != 0 ||
        MH_CreateHook(at, (void*)&go_on_adventure, (void**)&g.o_go) != MH_OK ||
        MH_EnableHook(at) != MH_OK) {
        g.o_go = nullptr;
        log_line("STARTRUN", "!! the box of cats moved -- the other player picks collars in the mod's window");
    }
}

void startrun_house_tick() {
    if (!g.pending) return;
    if (GetTickCount64() - g.pending_at > kStaleMs) {
        g.pending = false;
        log_line("STARTRUN", "!! the host's start waited too long for this game's House -- dropped");
        return;
    }
    const char* step = nullptr;
    if (!leave_on_house() || leave_loading(&step)) return;
    g.pending = false;
    go_after_host();
}

bool startrun_on_message(const ChoiceMsg& m) {
    if (m.kind != kChoiceRunParty && m.kind != kChoiceRunStart && m.kind != kChoiceRunCollar &&
        m.kind != kChoiceRunCutscene)
        return false;
    if (net_role() == NetRole::Host) return true;
    if (m.kind == kChoiceRunCutscene) {
        strncpy_s(g.cut_kind, m.name, _TRUNCATE);
        g.cut_pending = true;
        g.cut_at      = GetTickCount64();
        log_line("STARTRUN", "<- the host's '%s' cutscene -- played at this game's next screen update", m.name);
        return true;
    }
    if (m.kind == kChoiceRunParty) {
        if (m.index == 0) g.party_n = 0;
        if (m.index < kMaxParty) {
            g.party[m.index] = m.cat;
            if (m.index + 1 > g.party_n) g.party_n = m.index + 1;
        }
    } else if (m.kind == kChoiceRunCollar) {
        if (m.index == 0) g.collars_n = 0;
        if (m.index < kMaxCollars) {
            strncpy_s(g.collars[m.index], m.name, _TRUNCATE);
            if (m.index + 1 > g.collars_n) g.collars_n = m.index + 1;
        }
    } else {
        strncpy_s(g.map, m.name, _TRUNCATE);
        g.pending    = g.o_go != nullptr;
        g.pending_at = GetTickCount64();
        log_line("STARTRUN", "<- the host confirmed its box of cats ('%s', %u cat(s), %u collar(s))"
                             " -- following from the House", g.map, g.party_n, g.collars_n);
    }
    return true;
}

} // namespace mgmp
