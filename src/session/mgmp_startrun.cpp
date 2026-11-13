#include "mgmp_startrun.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"
#include "mgmp_leave.h"
#include "mgmp_catsync.h"
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
// MewDirector::DefendHouse(std::string, CustomVectorInterface<int64> cats),
// sub_1403B1680, the same box's other way out (sub_1400AE360): the cats go to
// hold the House, through the gear screen and no chooser. Followed the same way.
constexpr uintptr_t kRva_DefendHouse = 0x3B1680;
const uint8_t kDefendPrologue[8] = { 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54 };
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
using fn_defend  = void* (__fastcall*)(void*, void*, void*);
using fn_play    = void* (__fastcall*)(void*, void*, void*, double);
using fn_alloc   = void* (__cdecl*)(size_t);
using fn_realloc = void* (__cdecl*)(void*, size_t);

struct State {
    uintptr_t  base_    = 0;
    fn_go      o_go     = nullptr;
    fn_defend  o_defend = nullptr;
    bool       defend   = false;    // client: the pending start is a defense
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

// THE BOX OF CATS. glaiel::ButchBox (vtable 0xEFCCC0) holds the cats picked for
// a run in four slots, a CustomVector<HouseCat*> at +0x100 (nulls for empty,
// made by its init 0x1400AD740), and its confirm (0x1400ADBF0) reads each
// slot's key at HouseCat+0x80. Slot 31 of the table puts a HouseCat in
// (0x1400AED00, this, cat), slot 32 takes it out (0x1400AF960, this, cat),
// slot 11 runs every frame (0x1400AE9D0). The House's cats are the scene's
// component list 0x448: box+0x18 -> +8 the scene, +0x20 -> +0x4480 a list
// {u32 cap?, u32 count at +0xC, HouseCat** at +0x10} (read so at 0x1400ADE0B).
//
// Each game's box is its own, so either player's picks go to the other by key
// and the other's game puts the same cat in or takes it out with its own calls,
// in the box's own update. The host's box is the one the run is started from.
constexpr uintptr_t kRva_BoxVtbl   = 0xEFCCC0;
constexpr uint32_t  kBoxTickSlot   = 11;
constexpr uint32_t  kBoxAddSlot    = 31;
constexpr uint32_t  kBoxRemoveSlot = 32;
constexpr uintptr_t kRva_BoxTick   = 0xAE9D0;
constexpr uintptr_t kRva_BoxAdd    = 0xAED00;
constexpr uintptr_t kRva_BoxRemove = 0xAF960;
constexpr uintptr_t kBox_Slots     = 0x100;
constexpr uintptr_t kBox_Owner     = 0x18;
constexpr uintptr_t kOwner_Scene   = 0x8;
constexpr uintptr_t kScene_Comps   = 0x20;
constexpr uintptr_t kComps_HouseCats = 0x4480;
constexpr uintptr_t kHouseCat_Key  = 0x80;
constexpr uint32_t  kBoxSlots      = 4;

// THE CONTAINER A CAT IS IN. Slot 31 alone moved the other player's cat's model
// into the box and left the cat walking the House in it (2026-11-14): the cat
// keeps its holder at HouseCat+0xE8, and only the container's own add writes
// it. The drop (0x1401FF140) clears +0xE8, finds the container under the
// cursor -- a ButchBox, a CatPlacementArea (a room) or a HousePipe -- and calls
// 0x1402E88D0 (container, cat): slot 31, then out of its old container through
// 0x1402E89C0 (container, cat: slot 32 and off the list at +0x6C/+0x70), then
// onto this one's list and +0xE8 = container. The other player's picks go the
// same way here, and a cat taken out goes back to the room it came from.
constexpr uintptr_t kRva_ContAdd    = 0x2E88D0;
constexpr uintptr_t kRva_ContRemove = 0x2E89C0;
constexpr uintptr_t kRva_RoomVtbl   = 0xEFCF10;   // CatPlacementArea
constexpr uintptr_t kRva_PipeVtbl   = 0xEFC9C0;   // HousePipe
constexpr uintptr_t kHouseCat_Holder = 0xE8;
const uint8_t kContAddHead[8]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
const uint8_t kContRemoveHead[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };

// EACH PLAYER'S SHARE (the user's ask, 2026-11-14). The cats a player put in
// the box are that player's for the run, and a player puts in at most:
//   2 players  the friend 2, the host the rest
//   3 players  the host 2, each friend 1
//   4 players  1 each
// Who put a cat in rides in the message's aux, as that player's position + 1
// (0 from an older build: the friend's on the host, the host's on a friend).
uint32_t box_cap(uint8_t pos, uint32_t players) {
    if (players <= 2) return pos == 0 ? kBoxSlots : 2;
    if (players == 3) return pos == 0 ? 2 : 1;
    return 1;
}

using fn_box = void* (__fastcall*)(void*, void*, void*, void*);
fn_box   o_box_tick = nullptr;
fn_box   box_add    = nullptr;
fn_box   box_remove = nullptr;
uint64_t box_known[kBoxSlots] = {};   // the keys in the box as last told to the other player
void*    box_seen   = nullptr;        // the box those were read off
using fn_cont = void (__fastcall*)(void*, void*);
fn_cont  cont_add    = nullptr;
fn_cont  cont_remove = nullptr;
fn_box   o_box_add   = nullptr;       // slot 31 as the game has it
uintptr_t box_base   = 0;
bool     box_applying = false;        // putting the other player's pick in: no limit
struct Picker { uint64_t key; uint8_t pos; };
Picker   pickers[kBoxSlots * 2] = {};   // who put each cat in the box
struct Prev { uint64_t key; void* holder; };
Prev     box_prev[8] = {};            // where the other player's picks were before the box
uint32_t box_prev_at = 0;
uint64_t box_out_key = 0, box_out_at = 0;   // this player's last cat out, against a stale resend
volatile uint64_t box_tick_at = 0;
volatile uint32_t box_note_mine = 0, box_note_any = 0;
struct BoxOp { uint64_t key; bool add; uint8_t pos; };
BoxOp    box_ops[16];
uint32_t box_op_n = 0;
CRITICAL_SECTION box_cs;
bool     box_cs_ready = false;

bool box_keys(void* box, void* cats[kBoxSlots], uint64_t keys[kBoxSlots]) {
    void* slots = nullptr;
    if (!mem_read((const uint8_t*)box + kBox_Slots, &slots, sizeof(slots)) || !slots) return false;
    if (!mem_read(slots, cats, sizeof(void*) * kBoxSlots)) return false;
    for (uint32_t i = 0; i < kBoxSlots; ++i) {
        keys[i] = 0;
        if (cats[i]) mem_read((const uint8_t*)cats[i] + kHouseCat_Key, &keys[i], sizeof(keys[i]));
    }
    return true;
}

bool key_in(const uint64_t keys[kBoxSlots], uint64_t k) {
    for (uint32_t i = 0; i < kBoxSlots; ++i) if (keys[i] == k) return true;
    return false;
}

// The House's cat with this key, off the scene's component list.
void* house_cat(void* box, uint64_t key) {
    void* owner = nullptr; void* scene = nullptr; void* comps = nullptr; void* list = nullptr;
    if (!mem_read((const uint8_t*)box + kBox_Owner, &owner, sizeof(owner)) || !owner) return nullptr;
    if (!mem_read((const uint8_t*)owner + kOwner_Scene, &scene, sizeof(scene)) || !scene) return nullptr;
    if (!mem_read((const uint8_t*)scene + kScene_Comps, &comps, sizeof(comps)) || !comps) return nullptr;
    if (!mem_read((const uint8_t*)comps + kComps_HouseCats, &list, sizeof(list)) || !list) return nullptr;
    uint32_t n = 0; void** data = nullptr;
    if (!mem_read((const uint8_t*)list + 0xC, &n, sizeof(n)) ||
        !mem_read((const uint8_t*)list + 0x10, &data, sizeof(data)) || !data || n > 4096)
        return nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        void* c = nullptr; uint64_t k = 0;
        if (mem_read(&data[i], &c, sizeof(c)) && c &&
            mem_read((const uint8_t*)c + kHouseCat_Key, &k, sizeof(k)) && k == key)
            return c;
    }
    return nullptr;
}

bool safe_cont_call(fn_cont f, void* cont, void* cat) {
    __try { f(cont, cat); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

constexpr uint32_t kPickers = kBoxSlots * 2;

int picker_of(uint64_t k) {
    for (uint32_t i = 0; i < kPickers; ++i)
        if (k && pickers[i].key == k) return pickers[i].pos;
    return -1;
}

void mark_picker(uint64_t k, int pos) {   // pos < 0: nobody's now
    for (uint32_t i = 0; i < kPickers; ++i)
        if (pickers[i].key == k) {
            if (pos < 0) pickers[i] = Picker{};
            else pickers[i].pos = (uint8_t)pos;
            return;
        }
    if (pos < 0 || !k) return;
    for (uint32_t i = 0; i < kPickers; ++i)
        if (!pickers[i].key) { pickers[i] = Picker{ k, (uint8_t)pos }; return; }
}

uint32_t picked_by(uint8_t pos, const uint64_t keys[kBoxSlots]) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < kBoxSlots; ++s)
        if (keys[s] && picker_of(keys[s]) == (int)pos) ++n;
    return n;
}

void send_box(bool add, uint64_t key, int pos) {
    ChoiceMsg m{};
    m.kind  = kChoiceBoxCat;
    m.index = add ? 1 : 0;
    m.cat   = key;
    m.aux   = pos >= 0 ? (uint32_t)pos + 1 : 0;
    net_send_choice(m);
}

void remember_prev(uint64_t key, void* holder) {
    for (uint32_t i = 0; i < 8; ++i)
        if (box_prev[i].key == key) { box_prev[i].holder = holder; return; }
    box_prev[box_prev_at++ % 8] = Prev{ key, holder };
}

// OUT OF THE BOX'S REACH. The box's slot 10 (0x1400AE7C0) takes, every frame,
// any House cat with no holder and not in a hand whose point lies inside the
// box's "bounds" clip -- slot 28 (0x1400AEB10, box, double out[4]: x min, x
// max, y min, y max). The point is transform(+0x40)+0x80/+0x88 plus
// transform+0x98/+0xA0 scaled by (cat+0x100)+0xC8/+0xD0, as it reads it. A cat
// taken out of the box here stood where it sat, and the box had it back the
// next frame (2026-11-14: every one of the friend's cats bounced back on the
// host). So it is set down just left of the bounds, free to walk off.
using fn_bounds = double* (__fastcall*)(void*, double*);
constexpr uint32_t kBoxBoundsSlot = 28;
constexpr double   kOutMargin     = 60.0;

bool safe_bounds(void* box, double out[4]) {
    __try {
        void** vt = *(void***)box;
        fn_bounds f = (fn_bounds)vt[kBoxBoundsSlot];
        double* r = f(box, out);
        if (r != out && r) memcpy(out, r, sizeof(double) * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void set_outside(void* box, void* cat) {
    double b[4] = {};
    uint8_t* t = nullptr; uint8_t* s = nullptr;
    if (!safe_bounds(box, b) || !(b[1] > b[0]) ||
        !mem_read((const uint8_t*)cat + 0x40, &t, sizeof(t)) || !t ||
        !mem_read((const uint8_t*)cat + 0x100, &s, sizeof(s)) || !s)
        return;
    double x = 0, ox = 0, sx = 0;
    if (!mem_read(t + 0x80, &x, sizeof(x)) || !mem_read(t + 0x98, &ox, sizeof(ox)) ||
        !mem_read(s + 0xC8, &sx, sizeof(sx)))
        return;
    const double pt = x + ox * sx;
    if (pt < b[0] - 1.0) return;                     // already clear of it
    const double nx = x + (b[0] - kOutMargin - pt);
    mem_write(t + 0x80, &nx, sizeof(nx));
}

// A room or a pipe this House still has: its table is one of theirs.
bool is_room(void* holder) {
    uintptr_t vt = 0;
    return holder && mem_read(holder, &vt, sizeof(vt)) &&
           (vt == box_base + kRva_RoomVtbl || vt == box_base + kRva_PipeVtbl);
}

void* prev_of(uint64_t key) {
    for (uint32_t i = 0; i < 8; ++i)
        if (box_prev[i].key == key) return box_prev[i].holder;
    return nullptr;
}

// Slot 31: a cat past this player's share is not taken. The drop has cleared
// the cat's holder already, so it stays where it was let go, as off any room.
void* __fastcall box_add_hook(void* box, void* cat, void* c, void* d) {
    const uint32_t players = net_peer_count();
    if (!box_applying && net_active() && players >= 2) {
        const uint8_t me = net_peer_pos();
        const uint32_t cap = box_cap(me, players);
        void* cats[kBoxSlots]; uint64_t keys[kBoxSlots];
        uint64_t k = 0;
        if (cat) mem_read((const uint8_t*)cat + kHouseCat_Key, &k, sizeof(k));
        if (box_keys(box, cats, keys) && !key_in(keys, k) && picked_by(me, keys) >= cap) {
            static uint64_t said = 0;
            if (GetTickCount64() - said > 3000) {
                said = GetTickCount64();
                log_line("STARTRUN", "one more cat of this player's not taken -- %u is the most"
                                     " with %u players", cap, players);
            }
            // Let go over the box it is not in: out of its reach, or it is offered
            // to the box again every frame.
            void* held = nullptr;
            mem_read((const uint8_t*)cat + kHouseCat_Holder, &held, sizeof(held));
            if (!held) set_outside(box, cat);
            return nullptr;
        }
    }
    return o_box_add(box, cat, c, d);
}

void* __fastcall box_tick(void* box, void* b, void* c, void* d) {
    void* r = o_box_tick(box, b, c, d);
    if (!box || !net_active() || net_peer_count() < 2) return r;
    if (box != box_seen) {
        box_seen = box;
        memset(box_known, 0, sizeof(box_known));
        memset(pickers, 0, sizeof(pickers));
        memset(box_prev, 0, sizeof(box_prev));
    }
    const bool host = net_role() == NetRole::Host;
    const uint8_t me = net_peer_pos();
    const uint64_t now = GetTickCount64();

    // The other player's picks first, by this game's own calls.
    BoxOp ops[16];
    uint32_t n = 0;
    if (box_cs_ready) EnterCriticalSection(&box_cs);
    n = box_op_n;
    memcpy(ops, box_ops, sizeof(BoxOp) * n);
    box_op_n = 0;
    if (box_cs_ready) LeaveCriticalSection(&box_cs);
    for (uint32_t i = 0; i < n; ++i) {
        void* cats[kBoxSlots]; uint64_t keys[kBoxSlots];
        if (!box_keys(box, cats, keys)) break;
        const bool in = key_in(keys, ops[i].key);
        if (ops[i].add && in && ops[i].pos != me) mark_picker(ops[i].key, ops[i].pos);
        if (ops[i].add == in) continue;               // already as the other player has it
        // The host's every-3-s box, sent before it heard this player take the cat out.
        if (ops[i].add && !host && ops[i].key == box_out_key && now - box_out_at < 2000) continue;
        void* hc = nullptr;
        if (ops[i].add) hc = house_cat(box, ops[i].key);
        else for (uint32_t s = 0; s < kBoxSlots; ++s) if (keys[s] == ops[i].key) hc = cats[s];
        if (!hc) {
            log_line("STARTRUN", "!! the other player's cat %016llx is not in this House --"
                                 " the box is left as it is", (unsigned long long)ops[i].key);
            continue;
        }
        bool ok = true;
        box_applying = true;
        if (ops[i].add) {
            void* was = nullptr;
            mem_read((const uint8_t*)hc + kHouseCat_Holder, &was, sizeof(was));
            if (was != box) remember_prev(ops[i].key, was);
            ok = safe_cont_call(cont_add, box, hc);
        } else {
            void* room = prev_of(ops[i].key);
            if (is_room(room)) {
                ok = safe_cont_call(cont_add, room, hc);   // out of the box on the way
            } else {
                ok = safe_cont_call(cont_remove, box, hc);
                void* none = nullptr;
                if (ok) mem_write((uint8_t*)hc + kHouseCat_Holder, &none, sizeof(none));
                if (ok) set_outside(box, hc);
            }
        }
        box_applying = false;
        box_keys(box, cats, keys);
        mark_picker(ops[i].key, ops[i].add && key_in(keys, ops[i].key) ? ops[i].pos : -1);
        log_line("STARTRUN", "<- the other player %s cat %016llx %s the box%s",
                 ops[i].add ? "put" : "took", (unsigned long long)ops[i].key,
                 ops[i].add ? "in" : "out of",
                 !ok ? " -- the game faulted on it" :
                 key_in(keys, ops[i].key) != ops[i].add ? " -- this game would not" : "");
        // Taken as the other player's, not told back to them.
        for (uint32_t s = 0; s < kBoxSlots; ++s) box_known[s] = keys[s];
    }

    // Then this player's: what changed since the other player was last told.
    void* cats[kBoxSlots]; uint64_t keys[kBoxSlots];
    if (!box_keys(box, cats, keys)) return r;
    for (uint32_t s = 0; s < kBoxSlots; ++s) {
        if (box_known[s] && !key_in(keys, box_known[s])) {
            send_box(false, box_known[s], me);
            log_line("STARTRUN", "-> took cat %016llx out of the box", (unsigned long long)box_known[s]);
            mark_picker(box_known[s], -1);
            box_out_key = box_known[s];
            box_out_at  = now;
        }
    }
    for (uint32_t s = 0; s < kBoxSlots; ++s) {
        if (keys[s] && !key_in(box_known, keys[s])) {
            send_box(true, keys[s], me);
            log_line("STARTRUN", "-> put cat %016llx in the box", (unsigned long long)keys[s]);
            mark_picker(keys[s], me);
        }
    }
    memcpy(box_known, keys, sizeof(box_known));
    // The host's box again every few seconds: a client whose House reloaded (a
    // new day) has an empty box and no change here to hear about.
    // For the note on the friend's screen.
    uint32_t any = 0;
    for (uint32_t s = 0; s < kBoxSlots; ++s) if (keys[s]) ++any;
    box_note_mine = picked_by(me, keys);
    box_note_any  = any;
    box_tick_at   = now;
    static uint64_t full_at = 0;
    if (host && now - full_at > 3000) {
        full_at = now;
        for (uint32_t s = 0; s < kBoxSlots; ++s)
            if (keys[s]) {
                const int p = picker_of(keys[s]);
                send_box(true, keys[s], p >= 0 ? p : 0);
            }
    }
    return r;
}

void watch_box(uintptr_t base) {
    void** slots = (void**)(base + kRva_BoxVtbl);
    void* tick = nullptr; void* add = nullptr; void* rem = nullptr;
    DWORD prot = 0;
    if (!mem_read(&slots[kBoxTickSlot], &tick, sizeof(tick)) ||
        !mem_read(&slots[kBoxAddSlot], &add, sizeof(add)) ||
        !mem_read(&slots[kBoxRemoveSlot], &rem, sizeof(rem)) ||
        (uintptr_t)tick != base + kRva_BoxTick || (uintptr_t)add != base + kRva_BoxAdd ||
        (uintptr_t)rem != base + kRva_BoxRemove ||
        memcmp((const void*)(base + kRva_ContAdd), kContAddHead, sizeof(kContAddHead)) ||
        memcmp((const void*)(base + kRva_ContRemove), kContRemoveHead, sizeof(kContRemoveHead)) ||
        !VirtualProtect(&slots[kBoxTickSlot], sizeof(void*) * (kBoxRemoveSlot - kBoxTickSlot + 1),
                        PAGE_READWRITE, &prot)) {
        log_line("STARTRUN", "!! the box of cats moved -- each player's box stays their own");
        return;
    }
    if (!box_cs_ready) { InitializeCriticalSection(&box_cs); box_cs_ready = true; }
    box_base    = base;
    cont_add    = (fn_cont)(base + kRva_ContAdd);
    cont_remove = (fn_cont)(base + kRva_ContRemove);
    o_box_add   = (fn_box)add;
    o_box_tick  = (fn_box)tick;
    slots[kBoxTickSlot] = (void*)&box_tick;
    slots[kBoxAddSlot]  = (void*)&box_add_hook;
    VirtualProtect(&slots[kBoxTickSlot], sizeof(void*) * (kBoxRemoveSlot - kBoxTickSlot + 1), prot, &prot);
}

void send_party(void* cats) {
    KeyVec kv{};
    uint32_t nk = 0;
    if (cats && mem_read(cats, &kv, sizeof(kv)) && kv.data) nk = kv.count;
    if (nk > kMaxParty) nk = kMaxParty;
    // The cats themselves first: the client's chooser copies them as it opens.
    {
        uint64_t keys[kMaxParty] = {};
        for (uint32_t i = 0; i < nk; ++i) mem_read(&kv.data[i], &keys[i], sizeof(keys[i]));
        catsync_push_keys(keys, nk, "the party, before the start");
    }
    for (uint32_t i = 0; i < nk; ++i) {
        uint64_t k = 0;
        mem_read(&kv.data[i], &k, sizeof(k));
        send(kChoiceRunParty, i, nk, k, nullptr);
    }
}

// HOST: the box of cats sent to hold the House.
void* __fastcall defend_house(void* dir, void* name, void* cats) {
    if (!g.ours && host_with_peer()) {
        char s[48];
        read_str(name, s, sizeof(s));
        send_party(cats);
        send(kChoiceRunDefend, 0, 0, 0, s);
        log_line("STARTRUN", "-> the host's cats defend the House ('%s') -- the other player's"
                             " game goes to its own gear screen", s);
    }
    return g.o_defend(dir, name, cats);
}

bool safe_defend(void* dir, GameStr* name, KeyVec* cats) {
    __try { g.o_defend(dir, name, cats); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
        send_party(cats);
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

void defend_after_host() {
    void* dir = director();
    if (!dir || !g.party_n || !g.o_defend || !g.realloc_) {
        log_line("STARTRUN", "!! the host's defense lacks something here -- not followed");
        return;
    }
    GameStr name{};
    void* keys = nullptr;
    if (!make_str(name, g.map) || !safe_realloc(g.party_n * sizeof(uint64_t), &keys) || !keys) {
        log_line("STARTRUN", "!! could not build the host's defense here");
        return;
    }
    memcpy(keys, g.party, g.party_n * sizeof(uint64_t));
    KeyVec cats{ g.party_n, g.party_n, (uint64_t*)keys };
    g.ours = true;
    const bool ok = safe_defend(dir, &name, &cats);
    g.ours = false;
    log_line("STARTRUN", ok ? "<- the host's cats defend the House ('%s', %u cat(s)) -- here too,"
                              " this game's gear screen opens"
                            : "!! following the host's defense faulted ('%s', %u cat(s))",
             g.map, g.party_n);
}

// CLIENT: the host's box of cats, confirmed in this game's House too.
void go_after_host() {
    if (g.defend) { defend_after_host(); return; }
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
    watch_box(base);
    void* dh = (void*)(base + kRva_DefendHouse);
    if (memcmp(dh, kDefendPrologue, 8) != 0 ||
        MH_CreateHook(dh, (void*)&defend_house, (void**)&g.o_defend) != MH_OK ||
        MH_EnableHook(dh) != MH_OK) {
        g.o_defend = nullptr;
        log_line("STARTRUN", "!! the House defense moved -- the other player does not follow it");
    }
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

int startrun_box_picker(uint64_t key) {
    return net_role() == NetRole::Host ? picker_of(key) : -1;
}

bool startrun_box_note(uint32_t& mine, uint32_t& most) {
    const uint32_t players = net_peer_count();
    if (!net_active() || players < 2) return false;
    const uint8_t me = net_peer_pos();
    if (me == 0 && players == 2) return false;    // the host of two has no share to count
    if (GetTickCount64() - box_tick_at > 500 || !box_note_any) return false;
    mine = box_note_mine;
    most = box_cap(me, players);
    return true;
}

bool startrun_on_message(const ChoiceMsg& m) {
    if (m.kind == kChoiceBoxCat) {
        if (!box_cs_ready || !m.cat) return true;
        EnterCriticalSection(&box_cs);
        const uint8_t pos = m.aux ? (uint8_t)(m.aux - 1)
                                  : (uint8_t)(net_role() == NetRole::Host ? 1 : 0);
        if (box_op_n < 16) box_ops[box_op_n++] = BoxOp{ m.cat, m.index == 1, pos };
        LeaveCriticalSection(&box_cs);
        return true;
    }
    if (m.kind != kChoiceRunParty && m.kind != kChoiceRunStart && m.kind != kChoiceRunCollar &&
        m.kind != kChoiceRunCutscene && m.kind != kChoiceRunDefend)
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
    } else if (m.kind == kChoiceRunDefend) {
        strncpy_s(g.map, m.name, _TRUNCATE);
        g.defend     = true;
        g.pending    = g.o_defend != nullptr;
        g.pending_at = GetTickCount64();
        log_line("STARTRUN", "<- the host's cats defend the House ('%s', %u cat(s)) -- following"
                             " from the House", g.map, g.party_n);
    } else {
        strncpy_s(g.map, m.name, _TRUNCATE);
        g.defend     = false;
        g.pending    = g.o_go != nullptr;
        g.pending_at = GetTickCount64();
        log_line("STARTRUN", "<- the host confirmed its box of cats ('%s', %u cat(s), %u collar(s))"
                             " -- following from the House", g.map, g.party_n, g.collars_n);
    }
    return true;
}

} // namespace mgmp
