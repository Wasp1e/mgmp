#include "mgmp_classpick.h"
#include "mgmp_lockstep.h"
#include "mgmp_catsync.h"
#include "mgmp_invsync.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_resolve.h"
#include "mgmp_follow.h"
#include "mgmp_startrun.h"
#include "mgmp_leave.h"
#include "mgmp_savefile.h"
#include "mgmp_combatlock.h"
#include "MinHook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace mgmp {
void follow_embark();   // below, outside this file's anonymous namespace
namespace {

// Read off Mewgenics.exe 1.1.21239 on 2026-11-03, statically; nothing here has
// been watched running yet, so every hook checks the slot it replaces.
//
// ClassTagBox::update (vtable 0xEE2130, slot 11) runs every frame for every
// collar while the chooser is up, which is how the chooser is found and how its
// going away is seen. Its click, std::function _Do_call at 0xEE2010+0x10, goes
// straight to sub_1401405A0(ClassTagBox*): a free collar goes on the cat
// ClassChooser+0xD0 names and sub_1400BDC70 writes the class into its CatData.
// The embark button's _Do_call (0xEE2250+0x10) asks POPUP_TRY_EMBARK_COLORLESS
// when a cat has no collar and starts the run otherwise.
constexpr uintptr_t kRva_TagTickSlot   = 0xEE2130 + 11 * 8;
constexpr uintptr_t kRva_TagTick       = 0x1402A0;
constexpr uintptr_t kRva_TagClickSlot  = 0xEE2010 + 2 * 8;
constexpr uintptr_t kRva_TagClickCall  = 0x144520;
constexpr uintptr_t kRva_EmbarkSlot    = 0xEE2250 + 2 * 8;
constexpr uintptr_t kRva_Embark        = 0x141E40;
constexpr uintptr_t kRva_TagClick      = 0x1405A0;
const uint8_t kTagClickPrologue[8]     = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41 };

constexpr uintptr_t kTag_Chooser   = 0x38;   // ClassChooser*
constexpr uintptr_t kTag_Class     = 0x58;   // std::string, "Fighter"
constexpr uintptr_t kTag_Cat       = 0x90;   // the cat key wearing it, -1 when free
constexpr uintptr_t kCh_TagCount   = 0x9C;   // u32
constexpr uintptr_t kCh_Tags       = 0xA0;   // ClassTagBox**
constexpr uintptr_t kCh_CopyBegin  = 0xB8;   // std::vector<CatData>, the cats as they came
constexpr uintptr_t kCh_CopyEnd    = 0xC0;
constexpr uintptr_t kCh_Current    = 0xD0;   // the key of the cat on screen
constexpr uintptr_t kCat_Size      = 0xC58;
constexpr uintptr_t kCat_Uid       = 0x00;   // the id the owner table uses
constexpr uintptr_t kCat_Name      = 0x18;   // std::wstring
constexpr uintptr_t kCat_Key       = 0xC48;  // the registry key MewDirector's run list holds

// THE ITEMS. sub_1400CA2E0(CatData*, Item*, list, 0) is what the inventory
// screen's drop calls for an item out of the bag or the storage: it puts the
// item in its slot, takes it off both of the Inventory's lists, and puts what
// the slot held back on list `list`. The Inventory is the global at 0x13C7560,
// its lists std::list heads at +0x40 (the bag, 0) and +0x80 (the storage, 1),
// an Item at node+0x18 with its id at +0 and its key at +8. sub_1402E0030(Item*)
// is its slot kind, 0-4 head, face, neck, weapon, trinket, 5 a learned ability;
// sub_1400C4980(CatData*, kind) is true when that slot is locked. Equipped items
// are CatData+0x9B0 + kind * 0x60, the same Item layout.
constexpr uintptr_t kRva_Inventory  = 0x13C7560;
constexpr uintptr_t kRva_Equip      = 0xCA2E0;
constexpr uintptr_t kRva_ItemKind   = 0x2E0030;
constexpr uintptr_t kRva_SlotLocked = 0xC4980;
// sub_1400CA510(CatData*, Item*, list, bool): finds the slot wearing that item
// (by id) and takes it off onto list `list` -- the inventory screen's drop of a
// worn item back onto the bag or the storage.
constexpr uintptr_t kRva_Unequip    = 0xCA510;
const uint8_t kUnequipPrologue[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t   kUnequipList    = 1;   // the storage: a thing taken off stays home
const uint8_t kEquipPrologue[8]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
const uint8_t kKindPrologue[8]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x81 };
const uint8_t kLockedPrologue[8] = { 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57 };
constexpr uintptr_t kInv_List[2]  = { 0x40, 0x80 };
constexpr uintptr_t kNode_Item    = 0x18;
constexpr uintptr_t kItem_Key     = 0x08;     // std::string
constexpr uintptr_t kCat_Slots    = 0x9B0;
constexpr uintptr_t kSlot_Stride  = 0x60;
constexpr uint64_t  kScanMs       = 250;
// CatStats, seven int32 -- str, dex, con, int, spd, cha, lck. SerializeCatData
// writes three in a row through sub_14022F040: the base at +0x6F0, what
// level-ups add at +0x70C (LevelUpScreen::select_option's), and +0x728. A fresh
// cat's +0x70C is all zero -- that is what the menu showed on 2026-11-04 -- so a
// cat's stats are the first two summed.
constexpr uintptr_t kCat_Stats      = 0x6F0;
// MUTATIONS. The cat's tooltip (sub_1400E5500, "HOUSE_CAT_INFO_HAS_MUTATIONS")
// asks sub_1400CB690(cat, out) for them -- the CatData itself, as the battle
// does (0x81BD28, Character+0x88); the tooltip's own +0x8A8 is its object's
// CatData pointer, and +0x8A8 of a CatData is a string (2026-11-05). out is {u32 cap, u32 count,
// u64* parts}, each {u32 slot, u32 id}, and only the parts whose data has an
// effect are in it -- a cat with none gets an empty list and no buffer. The id
// is the part's number in the slot's file of data/mutations/ (700 on, a defect).
constexpr uintptr_t kRva_PartList   = 0xCB690;
const uint8_t kPartListPrologue[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };
constexpr uint32_t  kFirstMutation  = 300;
constexpr uintptr_t kCat_StatsBonus = 0x70C;

// THE SECOND SCREEN. Embark commits the chooser's own copies of the cats
// (+0xB8) back to the cats, so an item put on a cat while the chooser is up is
// lost -- taken off the lists, then written over: on 2026-11-04 every item the
// friend put on was gone on the next screen. That next screen is the game's
// inventory, InventoryScreen2 (vtable 0xF13D00), whose update (slot 10,
// 0x34D130) rebuilds which cat wears what from the cats each frame. The friend's
// gear is put on there, from inside that update, as the screen's own drop does.
constexpr uintptr_t kRva_InvScreenSlot = 0xF13D00 + 10 * 8;
constexpr uintptr_t kRva_InvScreenTick = 0x34D130;
constexpr uint64_t  kGearWaitMs = 60000;   // the gear screen has this long to come up

// WHICH CAT. The inventory screen finds its cats as the run does: the keys at
// MewDirector+0x5C0 looked up in the registry at +0x598 by sub_1400D7220
// (registry, key) -> CatData*, the key being CatData+0xC48 (sub_1403B2F30). The
// mod went by CatData+0 from the chooser's copies, and on 2026-11-04 the
// friend's item went on for one cat and showed up on another -- so it goes by
// the key, the screen's own way, and says the name of the cat it found.
constexpr uintptr_t kRva_CatLookup = 0xD7220;
constexpr uintptr_t kDir_Registry  = 0x598;
const uint8_t kLookupPrologue[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };

constexpr uint64_t kFree        = ~0ull;
constexpr uint64_t kGoneMs      = 500;
constexpr uint64_t kResendMs    = 2000;
constexpr uint64_t kStaleMs     = 6000;
constexpr uint64_t kEmbarkAgain = 10000;

using fn_this = void* (__fastcall*)(void*, void*, void*, void*);
using fn_click = void (__fastcall*)(void*);
using fn_equip  = bool (__fastcall*)(void*, void*, int, int);
using fn_kind   = int  (__fastcall*)(void*);
using fn_locked = bool (__fastcall*)(void*, int);
using fn_unequip = bool (__fastcall*)(void*, void*, int, int);
using fn_lookup  = void* (__fastcall*)(void*, uint64_t);
using fn_parts   = void* (__fastcall*)(void*, void*);

struct Pick { uint64_t key; uint32_t tag; char name[48]; bool state = false; };
struct Equip { uint64_t key; uint64_t item; uint8_t list; uint8_t kind; char name[48]; bool off; };

struct State {
    fn_this  o_tick   = nullptr;
    fn_this  o_click  = nullptr;
    fn_this  o_embark = nullptr;
    fn_click click    = nullptr;
    fn_equip  equip   = nullptr;
    fn_kind   kind    = nullptr;
    fn_locked locked  = nullptr;
    fn_unequip unequip = nullptr;
    uintptr_t base    = 0;

    void*    chooser  = nullptr;
    uint64_t seen     = 0;

    // host
    void*    claimed_for = nullptr;
    uint32_t cats        = 0;
    uint64_t key[kClassPickCats]   = {};
    uint64_t uid[kClassPickCats]   = {};
    uint8_t  owner[kClassPickCats] = {};
    char     name[kClassPickCats][48] = {};
    uint64_t sent_hash = 0;
    uint64_t sent_at   = 0;
    bool     open_sent = false;
    bool     applying  = false;
    uint64_t embark_at = 0;
    uint64_t said_cat  = 0;
    Pick     queue[8]  = {};
    uint32_t queued    = 0;
    Equip    equips[8] = {};
    uint32_t equipped  = 0;
    uint64_t scanned_at = 0;
    bool     said_screen = false;
    fn_this  o_inv      = nullptr;
    fn_this  o_confirm  = nullptr;
    fn_lookup lookup    = nullptr;
    uint64_t run_tried_at = 0;
    fn_parts parts      = nullptr;
    uint64_t embark_refused_at = 0;
    bool     embark_due = false;        // client: the host embarked; this chooser follows
    void*    embark_for = nullptr;      // ...for this chooser
    struct Gear { bool on; uint64_t cat; uint64_t item; uint8_t list; char name[48]; };
    Gear     gear[64] = {};             // client: the host's gear moves, for its screen
    uint32_t gear_n   = 0;
    void*    embark_closure[3] = {};    // kept: the popup it may ask holds on to it
    uint64_t wear_sent_at = 0;
    void*    embarking = nullptr;       // host: the chooser whose embark went through
    uint64_t embarking_at = 0;
    fn_equip   o_equip_hook   = nullptr;
    fn_unequip o_unequip_hook = nullptr;
    uint64_t mirrored_at = 0;
    uint64_t mut_key[kClassPickCats] = {};      // whose mutations muts[] holds
    char     muts[kClassPickCats][48] = {};
    uint64_t inv_at     = 0;     // the inventory screen's last update
    uint8_t  phase      = 0;     // what was last sent: 0 none, 1 collars, 2 gear
    uint64_t chooser_gone_at = 0;
    bool     gear_seen  = false;

    // client
    ClassPickView view, building;
    bool     active   = false;
    uint64_t last_end = 0;
};

State g;

bool chooser_up() { return g.chooser && GetTickCount64() - g.seen < kGoneMs; }

bool safe_lookup(void* reg, uint64_t key, void** out) {
    __try { *out = g.lookup(reg, key); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Cat `c` of the chooser, as the game has it now. See kRva_CatLookup.
void* run_cat(uint32_t c) {
    if (c >= g.cats || !g.key[c] || !g.lookup) return nullptr;
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    void* reg = nullptr;
    void* cat = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_Registry, &reg, sizeof(reg)) || !reg ||
        !safe_lookup(reg, g.key[c], &cat) || !cat)
        return nullptr;
    uint64_t key = 0;
    if (!mem_read((const uint8_t*)cat + kCat_Key, &key, sizeof(key)) || key != g.key[c])
        return nullptr;
    return cat;
}
bool inv_up() { return g.o_inv && GetTickCount64() - g.inv_at < kGoneMs; }
void apply_equips();

bool apply_gear();
bool run_gear();
uint64_t key_of(void* cat);

// What the gear screen's sort buttons call after re-sorting (the storage
// grid's sub_14034AA00, then sub_14034B620): the screen built again from the
// data, which is what the client's needs after the host's items moved in it --
// their move alone left the storage short and the cat's slots empty.
constexpr uintptr_t kRva_InvRebuild = 0x34AA00;
constexpr uintptr_t kRva_InvLayout  = 0x34B620;
using fn_screen = void (__fastcall*)(void*);
bool safe_rebuild(void* screen) {
    __try {
        ((fn_screen)(g.base + kRva_InvRebuild))(screen);
        ((fn_screen)(g.base + kRva_InvLayout))(screen);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// CLIENT: its gear screen's confirm, <lambda_1> of InventoryScreen2::init
// (vftable 0xF11D10, _Do_call sub_140351300 -> sub_14034D870), does nothing
// while the host's gear screen is up: confirmed here it set off this game's own
// fight or map, and the run or the fight is the host's, by its save. Staying
// on this screen is also where the host's embark cutscene plays.
constexpr uintptr_t kRva_GearConfirmSlot = 0xF11D10 + 2 * 8;
constexpr uintptr_t kRva_GearConfirm     = 0x351300;
void* __fastcall gear_confirm(void* a, void* b, void* c, void* d) {
    if (net_role() != NetRole::Host && net_active() && g.active && g.view.phase == 2) {
        log_line("CLASSPICK", "confirm pressed on this gear screen -- the host confirms, this game follows");
        return nullptr;
    }
    return g.o_confirm(a, b, c, d);
}

void dim_slots(void* screen);
void dim_tag(void* tag);

void* __fastcall inv_tick(void* a, void* b, void* c, void* d) {
    g.inv_at = GetTickCount64();
    if (net_role() != NetRole::Host) startrun_scene_tick();
    if (net_role() != NetRole::Host && g.gear_n && apply_gear() && !safe_rebuild(a))
        log_line("CLASSPICK", "!! rebuilding this game's gear screen faulted");
    // The friend's gear, put on from inside the screen's own update.
    if (net_role() == NetRole::Host && (g.phase == 2 || run_gear()) && g.equipped) {
        apply_equips();
        // The friend's items moved under this screen too: built again, as the
        // client's is (else they showed only at the host's next click).
        if (!safe_rebuild(a)) log_line("CLASSPICK", "!! rebuilding the gear screen faulted");
    }
    void* r = g.o_inv(a, b, c, d);
    dim_slots(a);
    return r;
}

void* __fastcall tag_tick(void* a, void* b, void* c, void* d) {
    void* ch = nullptr;
    if (mem_read((const uint8_t*)a + kTag_Chooser, &ch, sizeof(ch)) && ch) {
        g.chooser = ch;
        g.seen    = GetTickCount64();
    }
    void* r = g.o_tick(a, b, c, d);
    dim_tag(a);
    if (net_role() != NetRole::Host) startrun_scene_tick();
    // From inside the chooser's own update: the embark may ask its popup (a cat
    // with no collar), and a popup outside a scene's update faults -- the
    // client did at 0x14077DD5F on 2026-11-05, called from the frame's end.
    ::mgmp::follow_embark();
    return r;
}

void read_wstring(const void* ws, char* out, size_t cap) {
    out[0] = 0;
    uint64_t size = 0, room = 0;
    if (!mem_read((const uint8_t*)ws + 0x10, &size, sizeof(size)) ||
        !mem_read((const uint8_t*)ws + 0x18, &room, sizeof(room)))
        return;
    const void* data = ws;
    if (room > 7 && !mem_read(ws, &data, sizeof(data))) return;
    wchar_t w[48] = {};
    if (size > 47) size = 47;
    if (!data || !mem_read(data, w, (size_t)size * sizeof(wchar_t))) return;
    WideCharToMultiByte(CP_UTF8, 0, w, (int)size, out, (int)cap - 1, nullptr, nullptr);
    out[cap - 1] = 0;
}

uint32_t read_tags(void* ch, void** tags) {
    uint32_t n = 0;
    void**   p = nullptr;
    if (!mem_read((const uint8_t*)ch + kCh_TagCount, &n, sizeof(n)) ||
        !mem_read((const uint8_t*)ch + kCh_Tags, &p, sizeof(p)) || !p)
        return 0;
    if (n > kClassPickTags) n = kClassPickTags;
    return mem_read(p, tags, n * sizeof(void*)) ? n : 0;
}

uint64_t tag_cat(void* tag) {
    uint64_t k = kFree;
    mem_read((const uint8_t*)tag + kTag_Cat, &k, sizeof(k));
    return k;
}

uint64_t current_cat(void* ch) {
    uint64_t k = 0;
    mem_read((const uint8_t*)ch + kCh_Current, &k, sizeof(k));
    return k;
}

int cat_index(uint64_t key) {
    for (uint32_t i = 0; i < g.cats; ++i) if (g.key[i] == key) return (int)i;
    return -1;
}

bool friends_cat(uint64_t key) {
    const int i = cat_index(key);
    return i >= 0 && g.owner[i] != 0;
}

bool vote_on() { return net_active() && net_role() == NetRole::Host && net_peer_count() >= 2; }

// CLIENT, in its own chooser (mgmp_startrun.h): whose cat, by the host's list.
// -1 when the host has not said.
int view_mine(uint64_t key) {
    for (uint32_t i = 0; i < g.view.cats; ++i)
        if (g.view.cat_key[i] == key) return g.view.cat_mine[i] ? 1 : 0;
    return -1;
}

bool client_chooser() { return net_active() && net_role() != NetRole::Host && chooser_up(); }

void send(uint8_t kind, uint32_t index, uint32_t count, uint32_t aux, uint64_t cat,
          const char* name);

// CLIENT: what this cat wears in its own chooser, to the host.
void send_wear(uint64_t key) {
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
    char cls[48] = {};
    for (uint32_t t = 0; t < n; ++t)
        if (tag_cat(tags[t]) == key)
            mem_read_std_string((const uint8_t*)tags[t] + kTag_Class, cls, sizeof(cls));
    send(kChoiceClassWear, 0, n, 0, key, cls);
    log_line("CLASSPICK", "-> cat %016llx wears %s", (unsigned long long)key,
             cls[0] ? cls : "no collar");
}

int tag_index(void* tag) {
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
    for (uint32_t t = 0; t < n; ++t) if (tags[t] == tag) return (int)t;
    return -1;
}

// The host's own click on a collar. The friend's cats are the friend's to dress.
void* __fastcall tag_click(void* a, void* b, void* c, void* d) {
    if (!g.applying && client_chooser()) {
        // This player's click in its own chooser: its own cats only, and the host
        // is told, as from the mod's window.
        const uint64_t cur = current_cat(g.chooser);
        void* tag = nullptr;
        mem_read((const uint8_t*)a + 8, &tag, sizeof(tag));
        const uint64_t on = tag ? tag_cat(tag) : kFree;
        const int t = tag ? tag_index(tag) : -1;
        if (view_mine(cur) != 1 || (on != kFree && on != cur && view_mine(on) != 1) || t < 0) {
            if (g.said_cat != cur) {
                g.said_cat = cur;
                log_line("CLASSPICK", "that cat is the host's -- the host picks its collar");
            }
            return nullptr;
        }
        void* r = g.o_click(a, b, c, d);
        send_wear(cur);
        return r;
    }
    if (!g.applying && vote_on() && chooser_up()) {
        const uint64_t cur = current_cat(g.chooser);
        void* tag = nullptr;
        mem_read((const uint8_t*)a + 8, &tag, sizeof(tag));
        // A worn collar's click takes it off whoever wears it (sub_1401405A0's
        // second half), so the friend's collar is the friend's too.
        const uint64_t on = tag ? tag_cat(tag) : kFree;
        if (friends_cat(cur) || (on != kFree && friends_cat(on))) {
            if (g.said_cat != cur) {
                g.said_cat = cur;
                log_line("CLASSPICK", "that cat is the other player's -- they pick its collar");
            }
            return nullptr;
        }
    }
    return g.o_click(a, b, c, d);
}

// Embarking before the friend has dressed their cats takes a second click.
// The client's own chooser never embarks: the run is the host's, its save follows.
void* __fastcall embark(void* a, void* b, void* c, void* d) {
    if (client_chooser() && !g.embark_due) {
        g.embark_refused_at = GetTickCount64();
        log_line("CLASSPICK", "embark pressed here -- the host confirms, this game follows");
        return nullptr;
    }
    if (vote_on() && chooser_up()) {
        void* tags[kClassPickTags];
        const uint32_t n = read_tags(g.chooser, tags);
        uint32_t bare = 0;
        for (uint32_t i = 0; i < g.cats; ++i) {
            if (!g.owner[i]) continue;
            bool dressed = false;
            for (uint32_t t = 0; t < n; ++t) dressed |= tag_cat(tags[t]) == g.key[i];
            bare += !dressed;
        }
        const uint64_t now = GetTickCount64();
        if (bare && now - g.embark_at > kEmbarkAgain) {
            g.embark_at = now;
            log_line("CLASSPICK", "%u of the other player's cats have no collar yet -- "
                                  "press embark again to go anyway", bare);
            return nullptr;
        }
    }
    // From here the chooser hands its classes to the cats and frees its collars;
    // a list read now says no cat wears one, and the client took every host
    // cat's collar off by it (2026-11-05). So nothing more goes out of this one.
    if (vote_on() && chooser_up()) { g.embarking = g.chooser; g.embarking_at = GetTickCount64(); }
    return g.o_embark(a, b, c, d);
}

bool patch(uintptr_t base, uintptr_t slot_rva, uintptr_t want, void* ours, fn_this& orig,
           const char* what) {
    void** slot = (void**)(base + slot_rva);
    void*  have = nullptr;
    DWORD  prot = 0;
    if (!mem_read(slot, &have, sizeof(have)) || (uintptr_t)have != base + want ||
        !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) {
        log_line("CLASSPICK", "!! %s moved -- the other player cannot pick collars", what);
        return false;
    }
    orig  = (fn_this)have;
    *slot = ours;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    return true;
}

// EACH PLAYER DRESSES THEIR OWN CATS. The inventory screen asks
// sub_1400C4980(cat, slot) whether a slot is locked, for every slot as it draws
// (0x34D1C1...) and again before taking an item off (0x34FA93), and a locked
// slot takes nothing on and gives nothing up. Asked from that screen about the
// other player's cat, the answer here is "locked" -- so the game itself keeps
// the items where they are, nothing is half moved. The mod's own calls
// (classpick, the host putting on the friend's picks) come from elsewhere and
// get the game's answer.
constexpr uintptr_t kInvCodeFrom = 0x34B000;
constexpr uintptr_t kInvCodeTo   = 0x351000;
fn_locked o_locked_hook = nullptr;
uint64_t  said_locked   = 0;

int view_mine(uint64_t key);
bool host_gear_screen();

bool __fastcall slot_locked(void* cat, int slot) {
    const bool r = o_locked_hook(cat, slot);
    if (r || !cat || !net_active()) return r;
    const uintptr_t ret = (uintptr_t)_ReturnAddress() - g.base;
    if (ret < kInvCodeFrom || ret >= kInvCodeTo) return r;
    // Before the run the host dresses every cat: none of the host's slots is
    // locked. The client's are refused at the equip itself, not drawn crossed.
    if (net_role() != NetRole::Host && g.active && g.view.phase == 2) return r;
    if (host_gear_screen()) return r;
    if (run_gear()) return r;   // refused at the equip, as before the run
    uint64_t uid = 0;
    if (!mem_read(cat, &uid, sizeof(uid)) || !lockstep_cat_theirs(uid)) return r;
    if (said_locked != uid) {
        said_locked = uid;
        char name[48] = {};
        read_wstring((const uint8_t*)cat + kCat_Name, name, sizeof(name));
        log_line("CLASSPICK", "'%s' is the other player's cat -- its gear is theirs to change", name);
    }
    return true;
}

// CLIENT, its own gear screen before the run: what this player puts on or takes
// off goes to the host, as from the mod's window.
bool safe_kind(void* item, int* out);

// HOST, its gear screen before a run: each item put on or taken off, to the
// client, whose screen does the same with the same item.
bool host_gear_screen() {
    return net_role() == NetRole::Host && net_active() && net_peer_count() >= 2 && g.phase == 2;
}

// CLIENT, its gear screen before a run: only the host dresses the cats; a drop
// here is refused and the item goes back (the screen checks the answer).
bool client_watches(uintptr_t ret) {
    return ret >= kInvCodeFrom && ret < kInvCodeTo && net_role() != NetRole::Host &&
           net_active() && g.active && g.view.phase == 2;
}

// IN A RUN the backpack goes the way the gear screen before it does: each move
// as it is made, each player dressing their own cats, the host's game moving
// every item. The whole bag sent once the host's backpack closed made the
// friend wait for the host (2026-11-06).
bool in_a_run() { return leave_in_run() || savefile_adventure_is_loaded(); }

bool run_gear() {
    // Not lockstep_in_battle() alone: its battle stays "current" on the map
    // until the next one, so after the first fight the backpack went back to the
    // whole-bag sync, crosses and all (2026-11-07). A fight is not on the Map
    // scene.
    // Read a few times a second, not on each of the slot checks of a frame.
    static uint64_t at = 0;
    static bool     on_map = false;
    const uint64_t  now = GetTickCount64();
    if (now - at > 200) { at = now; on_map = leave_scene_up("Map"); }
    const bool fighting = combatlock_menu_live() || (lockstep_in_battle() && !on_map);
    return net_active() && net_peer_count() >= 2 && in_a_run() && !fighting &&
           (net_role() == NetRole::Host ? g.phase != 2 : !(g.active && g.view.phase == 2));
}

// 1 this player's cat in the run, 0 the other player's, -1 neither.
int run_owner(void* cat) {
    uint64_t uid = 0;
    if (!cat || !mem_read(cat, &uid, sizeof(uid)) || !uid) return -1;
    if (lockstep_cat_theirs(uid)) return 0;
    return lockstep_cat_known(uid) ? 1 : -1;
}

// CLIENT: a move onto or off its own cat, asked of the host.
void ask_equip(void* cat, void* item, int list) {
    int kind = -1;
    if (!cat || !item || !safe_kind(item, &kind)) return;
    ChoiceMsg m{};
    m.kind      = kChoiceClassEquip;
    m.cat       = key_of(cat);
    mem_read(item, &m.node_seed, sizeof(m.node_seed));
    m.aux       = (uint32_t)(kind & 0xFF) | ((uint32_t)(list & 0xFF) << 8);
    mem_read_std_string((const uint8_t*)item + kItem_Key, m.name, sizeof(m.name));
    net_send_choice(m);
    log_line("CLASSPICK", "-> asked the host to put '%s' on cat %016llx", m.name,
             (unsigned long long)m.cat);
}
void ask_unequip(void* cat, void* item, int list) {
    int kind = -1;
    if (!cat || !item || !safe_kind(item, &kind) || kind < 0 || kind >= (int)kClassPickSlots) return;
    ChoiceMsg m{};
    m.kind = kChoiceClassUnequip;
    m.cat  = key_of(cat);
    m.aux  = (uint32_t)kind | ((uint32_t)((list & 0xFF) + 1) << 8);   // +1: a list given
    net_send_choice(m);
    log_line("CLASSPICK", "-> asked the host to take slot %d off cat %016llx", kind,
             (unsigned long long)m.cat);
}

bool safe_kind(void* item, int* out);
int cat_index(uint64_t key);

uint64_t key_of(void* cat) {
    uint64_t key = 0;
    if (cat) mem_read((const uint8_t*)cat + kCat_Key, &key, sizeof(key));
    return key;
}

// The friend's cat, on the host's side.
bool friends_on_host(void* cat) {
    const int ci = cat_index(key_of(cat));
    return ci >= 0 && g.owner[ci];
}

// THE OTHER PLAYER'S CAT, DIMMED. The screen's update (slot 10, sub_14034D130)
// reads the cat it shows from +0x108 (a registry key) and shows or hides the
// "lock_<slot>" clips in two views, +0x40 and +0x48: UI objects whose +0x38 is
// their node map (sub_14005A100 by name, a const std::string&). Those clips sit
// in the slots' panel (the UI swf's sprite 3652: the frames and the locks), so
// that panel's node -- a lock's parent, node+0x38 -- is darkened through its
// colour transform: node+0x78, mul RGBA then add RGBA, floats (the node's
// constructor, sub_1409B08C0, sets 1,1,1,1, 0,0,0,0).
constexpr uintptr_t kRva_NodeByName = 0x5A100;
constexpr size_t    kInv_CatKey  = 0x108;
constexpr size_t    kInv_View[2] = { 0x40, 0x48 };
constexpr size_t    kView_Nodes  = 0x38;
constexpr size_t    kNode_Parent = 0x38;
constexpr size_t    kNode_Color  = 0x78;
constexpr float     kDim         = 0.4f;
using fn_node_by_name = void* (__fastcall*)(void*, const void*);
void* cat_by_key(uint64_t key);

struct MsString { char buf[16]; uint64_t size; uint64_t cap; };

void* safe_node(void* map, const MsString* s) {
    __try { return ((fn_node_by_name)(g.base + kRva_NodeByName))(map, s); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Whether the cat with this key is the other player's to dress, here and now.
bool theirs_to_dress(uint64_t key) {
    if (!key || !net_active()) return false;
    if (net_role() != NetRole::Host && g.active && g.view.phase == 2) return view_mine(key) == 0;
    void* cat = cat_by_key(key);
    if (!cat) return false;
    if (host_gear_screen()) return friends_on_host(cat);
    return run_gear() && run_owner(cat) == 0;
}

// The parent of the node named `child` in a UI object's node map (view+0x38),
// dimmed or back to full. True if it changed.
bool dim_parent_of(void* view, const char* child, bool dim) {
    const float want = dim ? kDim : 1.0f;
    MsString name{};
    strcpy_s(name.buf, child);
    name.size = strlen(name.buf);
    name.cap  = 15;
    void* map = nullptr;
    if (!view || !mem_read((const uint8_t*)view + kView_Nodes, &map, sizeof(map)) || !map)
        return false;
    void* node = safe_node(map, &name);
    void* panel = nullptr;
    if (!node || !mem_read((const uint8_t*)node + kNode_Parent, &panel, sizeof(panel)) || !panel)
        return false;
    float ct[8] = {};
    if (!mem_read((const uint8_t*)panel + kNode_Color, ct, sizeof(ct))) return false;
    // Only a transform this has set or the constructor's: one the game's own
    // tweens are running is left alone.
    const bool plain = (ct[0] == 1.0f || ct[0] == kDim) && ct[0] == ct[1] && ct[1] == ct[2] &&
                       ct[4] == 0.0f && ct[5] == 0.0f && ct[6] == 0.0f;
    if (!plain || ct[0] == want) return false;
    ct[0] = ct[1] = ct[2] = want;
    return mem_write((uint8_t*)panel + kNode_Color, ct, sizeof(float) * 3);
}

// THE CLASS CHOOSER, the same way: each collar's box (ClassTagBox, its update
// sub_1401402A0) keeps its picture in the UI object at +0x48, a "bg" node in
// its node map, whose parent is the box's. While the cat on screen
// (chooser+0xD0) is the other player's, every collar is dimmed: none is this
// player's to put on it.
constexpr size_t kTagBox_View = 0x48;

bool chooser_cat_theirs(uint64_t key) {
    if (!key || key == kFree || !net_active()) return false;
    if (net_role() != NetRole::Host) return g.active && view_mine(key) == 0;
    if (net_peer_count() < 2) return false;
    const int ci = cat_index(key);
    return ci >= 0 && g.owner[ci];
}

void dim_tag(void* tag) {
    static uint64_t said_key = 0;
    if (!tag || !g.base) return;
    void* ch = nullptr;
    void* view = nullptr;
    if (!mem_read((const uint8_t*)tag + kTag_Chooser, &ch, sizeof(ch)) || !ch ||
        !mem_read((const uint8_t*)tag + kTagBox_View, &view, sizeof(view)) || !view)
        return;
    const uint64_t key = current_cat(ch);
    const bool dim = chooser_cat_theirs(key);
    if (dim_parent_of(view, "bg", dim) && key != said_key) {
        said_key = key;
        log_line("CLASSPICK", "%s the collars for cat %016llx", dim ? "dimmed" : "undimmed",
                 (unsigned long long)key);
    }
}

void dim_slots(void* screen) {
    static uint64_t said_key = 0;
    static bool     said_dim = false;
    if (!screen || !g.base) return;
    uint64_t key = 0;
    if (!mem_read((const uint8_t*)screen + kInv_CatKey, &key, sizeof(key))) return;
    const bool dim = theirs_to_dress(key);
    bool changed = false;
    for (size_t v : kInv_View) {
        void* view = nullptr;
        if (mem_read((const uint8_t*)screen + v, &view, sizeof(view)) && view &&
            dim_parent_of(view, "lock_weapon", dim))
            changed = true;
    }
    if (changed && (key != said_key || dim != said_dim)) {
        said_key = key;
        said_dim = dim;
        log_line("CLASSPICK", "%s cat %016llx's slots", dim ? "dimmed the other player's" : "undimmed",
                 (unsigned long long)key);
    }
}

bool __fastcall equip_hook(void* cat, void* item, int list, int flag) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress() - g.base;
    // Each player dresses their own cats; the host's game alone moves an item,
    // so one is never in two places. The client's drop on its own cat is asked
    // of the host and shows when the host's answer comes back as a move.
    if (client_watches(ret)) {
        if (cat && item && view_mine(key_of(cat)) == 1) ask_equip(cat, item, list);
        return false;
    }
    const bool in_inv = ret >= kInvCodeFrom && ret < kInvCodeTo;
    const int  owner  = in_inv && run_gear() ? run_owner(cat) : -1;
    if (owner >= 0 && net_role() != NetRole::Host) {
        if (owner == 1) ask_equip(cat, item, list);
        return false;
    }
    if (owner == 0) return false;   // the host's backpack, the friend's cat
    if (in_inv && host_gear_screen() && friends_on_host(cat))
        return false;   // the friend's cat: theirs to dress
    const bool watch = cat && item && in_inv && (host_gear_screen() || owner == 1);
    uint64_t id = 0;
    char key[48] = {};
    if (watch) {
        mem_read(item, &id, sizeof(id));
        mem_read_std_string((const uint8_t*)item + kItem_Key, key, sizeof(key));
    }
    const bool ok = g.o_equip_hook(cat, item, list, flag);
    // The item's fields are read before: the equip moves it out of its list node.
    if (ok && watch) {
        ChoiceMsg m{};
        m.kind = kChoiceGearOn;
        mem_read((const uint8_t*)cat + kCat_Key, &m.cat, sizeof(m.cat));
        m.node_seed = id;
        strncpy_s(m.name, key, _TRUNCATE);
        m.aux = (uint32_t)(list & 0xFF);
        net_send_choice(m);
        log_line("CLASSPICK", "-> put '%s' on cat %016llx, for the other player's screen", key,
                 (unsigned long long)m.cat);
    }
    return ok;
}

bool __fastcall unequip_hook(void* cat, void* item, int list, int flag) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress() - g.base;
    if (client_watches(ret)) {
        if (cat && item && view_mine(key_of(cat)) == 1) ask_unequip(cat, item, list);
        return false;
    }
    const bool in_inv = ret >= kInvCodeFrom && ret < kInvCodeTo;
    const int  owner  = in_inv && run_gear() ? run_owner(cat) : -1;
    if (owner >= 0 && net_role() != NetRole::Host) {
        if (owner == 1) ask_unequip(cat, item, list);
        return false;
    }
    if (owner == 0) return false;
    if (in_inv && host_gear_screen() && friends_on_host(cat))
        return false;
    const bool watch = cat && item && in_inv && (host_gear_screen() || owner == 1);
    uint64_t id = 0;
    char key[48] = {};
    if (watch) {
        mem_read(item, &id, sizeof(id));
        mem_read_std_string((const uint8_t*)item + kItem_Key, key, sizeof(key));
    }
    const bool ok = g.o_unequip_hook(cat, item, list, flag);
    if (ok && watch) {
        ChoiceMsg m{};
        m.kind = kChoiceGearOff;
        mem_read((const uint8_t*)cat + kCat_Key, &m.cat, sizeof(m.cat));
        m.node_seed = id;
        strncpy_s(m.name, key, _TRUNCATE);
        m.aux = (uint32_t)(list & 0xFF);
        net_send_choice(m);
        log_line("CLASSPICK", "-> took '%s' off cat %016llx, for the other player's screen", key,
                 (unsigned long long)m.cat);
    }
    return ok;
}

void hook_gear(uintptr_t base) {
    void* e = (void*)(base + kRva_Equip);
    void* u = (void*)(base + kRva_Unequip);
    if (MH_CreateHook(e, (void*)&equip_hook, (void**)&g.o_equip_hook) != MH_OK ||
        MH_EnableHook(e) != MH_OK)
        log_line("CLASSPICK", "!! could not watch the gear screen's equip -- the client's own picks stay home");
    if (MH_CreateHook(u, (void*)&unequip_hook, (void**)&g.o_unequip_hook) != MH_OK ||
        MH_EnableHook(u) != MH_OK)
        log_line("CLASSPICK", "!! could not watch the gear screen's unequip");
}

void hook_slot_locked(uintptr_t base) {
    void* at = (void*)(base + kRva_SlotLocked);
    if (MH_CreateHook(at, (void*)&slot_locked, (void**)&o_locked_hook) != MH_OK ||
        MH_EnableHook(at) != MH_OK) {
        o_locked_hook = nullptr;
        log_line("CLASSPICK", "!! could not hook the slot lock -- either player can dress any cat");
    }
}

// --- the host ----------------------------------------------------------------

void claim(void* ch) {
    uint8_t* begin = nullptr;
    uint8_t* end   = nullptr;
    if (!mem_read((const uint8_t*)ch + kCh_CopyBegin, &begin, sizeof(begin)) ||
        !mem_read((const uint8_t*)ch + kCh_CopyEnd, &end, sizeof(end)) || !begin || end < begin)
        return;
    uint32_t n = (uint32_t)((end - begin) / kCat_Size);
    if (n > kClassPickCats) n = kClassPickCats;
    uint64_t uid[kClassPickCats] = {};
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* cat = begin + i * kCat_Size;
        if (!mem_read(cat + kCat_Key, &g.key[i], sizeof(g.key[i])) ||
            !mem_read(cat + kCat_Uid, &uid[i], sizeof(uid[i])))
            return;
        read_wstring(cat + kCat_Name, g.name[i], sizeof(g.name[i]));
    }
    g.cats = n;
    for (uint32_t i = 0; i < n; ++i) g.uid[i] = uid[i];
    lockstep_claim_cats(uid, n, g.owner);
    g.claimed_for = ch;
    for (uint32_t i = 0; i < n; ++i)
        log_line("CLASSPICK", "  cat '%s' (%016llx) -> player %u", g.name[i],
                 (unsigned long long)uid[i], (unsigned)g.owner[i]);
}

// GEAR WITH NO COLLARS FIRST. Cats that have a class already -- the retired
// ones sent to hold the House against the Guillotine, 2026-11-05 -- go straight
// to the inventory screen, and the friend saw nothing: its cats came from the
// chooser. Here they come from the run's own list, the one that screen shows.
bool claim_run() {
    uint64_t uid[kClassPickCats] = {};
    const uint32_t n = catsync_run_uids(uid, kClassPickCats);
    if (!n) return false;
    for (uint32_t i = 0; i < n; ++i) {
        void* cat = catsync_cat_by_uid(uid[i]);
        if (!cat || !mem_read((const uint8_t*)cat + kCat_Key, &g.key[i], sizeof(g.key[i])))
            return false;
        read_wstring((const uint8_t*)cat + kCat_Name, g.name[i], sizeof(g.name[i]));
    }
    g.cats = n;
    for (uint32_t i = 0; i < n; ++i) g.uid[i] = uid[i];
    lockstep_claim_cats(uid, n, g.owner);
    g.claimed_for = nullptr;
    log_line("CLASSPICK", "the gear screen came up with no collars before it -- %u cat(s) from"
                          " the run", n);
    for (uint32_t i = 0; i < n; ++i)
        log_line("CLASSPICK", "  cat '%s' (%016llx) -> player %u", g.name[i],
                 (unsigned long long)uid[i], (unsigned)g.owner[i]);
    return true;
}

void click_as(void* tag, uint64_t key) {
    const uint64_t cur = current_cat(g.chooser);
    mem_write((uint8_t*)g.chooser + kCh_Current, &key, sizeof(key));
    g.applying = true;
    g.click(tag);
    g.applying = false;
    mem_write((uint8_t*)g.chooser + kCh_Current, &cur, sizeof(cur));
    g.sent_hash = 0;
}

// The friend's cat `p.key` wears collar p.tag, or none (kNoCollar). Clicking the
// wanted collar swaps the old one off; none takes the worn one off.
void apply_wear(const Pick& p) {
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
    const int ci = cat_index(p.key);
    if (ci < 0 || !g.owner[ci]) {
        log_line("CLASSPICK", "!! refused a collar for cat %016llx -- not the other player's",
                 (unsigned long long)p.key);
        return;
    }
    int worn = -1;
    for (uint32_t t = 0; t < n; ++t) if (tag_cat(tags[t]) == p.key) worn = (int)t;
    if (!p.name[0]) {
        if (worn < 0) return;
        click_as(tags[worn], p.key);
        log_line("CLASSPICK", "<- '%s' wears no collar now", g.name[ci]);
        return;
    }
    // By class: the chooser keeps its collars in an order of its own (a worn one
    // leaves the grid), so an index means one thing here and another there.
    if (worn >= 0) {
        char cls[48] = {};
        mem_read_std_string((const uint8_t*)tags[worn] + kTag_Class, cls, sizeof(cls));
        if (strcmp(cls, p.name) == 0) return;
    }
    int want = -1;
    for (uint32_t t = 0; t < n && want < 0; ++t) {
        char cls[48] = {};
        mem_read_std_string((const uint8_t*)tags[t] + kTag_Class, cls, sizeof(cls));
        if (strcmp(cls, p.name) == 0 && tag_cat(tags[t]) == kFree) want = (int)t;
    }
    if (want < 0) {
        log_line("CLASSPICK", "no free '%s' collar here -- '%s' keeps its own", p.name, g.name[ci]);
        return;
    }
    click_as(tags[want], p.key);
    log_line("CLASSPICK", "<- '%s' wears the '%s' collar now", g.name[ci], p.name);
}

void apply(const Pick& p) {
    if (p.state) { apply_wear(p); return; }
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
    const int ci = cat_index(p.key);
    char have[48] = {};
    if (p.tag < n) mem_read_std_string((const uint8_t*)tags[p.tag] + kTag_Class, have, sizeof(have));
    if (ci < 0 || !g.owner[ci] || p.tag >= n || strcmp(have, p.name) != 0) {
        log_line("CLASSPICK", "!! refused the other player's collar '%s' for cat %016llx --"
                              " not their cat, or the collars moved", p.name,
                 (unsigned long long)p.key);
        return;
    }
    const uint64_t on = tag_cat(tags[p.tag]);
    // Worn by this cat: the same click takes it off.
    if (on != kFree && on != p.key) {
        log_line("CLASSPICK", "the '%s' collar is already on another cat", p.name);
        return;
    }
    // The click dresses the cat on screen; for the length of it, that is the
    // friend's cat.
    const uint64_t cur = current_cat(g.chooser);
    mem_write((uint8_t*)g.chooser + kCh_Current, &p.key, sizeof(p.key));
    g.applying = true;
    g.click(tags[p.tag]);
    g.applying = false;
    mem_write((uint8_t*)g.chooser + kCh_Current, &cur, sizeof(cur));
    log_line("CLASSPICK", on == p.key ? "<- the other player took the '%s' collar off '%s'"
                                      : "<- the other player put the '%s' collar on '%s'",
             p.name, g.name[ci]);
    g.sent_hash = 0;
}

struct PartVec { uint32_t cap; uint32_t count; uint64_t* data; };

bool safe_parts(void* genome, PartVec* out) {
    __try { g.parts(genome, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Cat `c`'s mutations into g.muts[c], once per cat: the list the game hands back
// is its own allocation and is left to it (a few bytes, once a cat).
void read_mutations(uint32_t c, void* cat) {
    if (g.mut_key[c] == g.key[c]) return;
    g.muts[c][0] = 0;
    void* genome = cat;
    PartVec v{};
    const char* why = !g.parts ? "no part list call"
                    : !cat ? "no cat"
                    : !safe_parts(genome, &v) ? "the part list faulted"
                    : v.count > 32 ? "too many parts" : nullptr;
    if (why) {
        // Once a cat; it is looked at again next time all the same.
        static uint64_t said[kClassPickCats] = {};
        if (said[c] != g.key[c]) {
            said[c] = g.key[c];
            log_line("CLASSPICK", "!! cat '%s': no mutations read -- %s (genome %p)", g.name[c], why,
                     genome);
        }
        return;
    }
    g.mut_key[c] = g.key[c];
    if (!v.count || !v.data) {
        log_line("CLASSPICK", "  cat '%s' has no mutations", g.name[c]);
        return;
    }
    char out[48] = {};
    char all[192] = {};
    size_t n = 0, na = 0;
    for (uint32_t i = 0; i < v.count; ++i) {
        uint64_t p = 0;
        if (!mem_read(&v.data[i], &p, sizeof(p))) break;
        const uint32_t slot = (uint32_t)p, id = (uint32_t)(p >> 32);
        na += _snprintf_s(all + na, sizeof(all) - na, _TRUNCATE, "%s%u:%u", na ? "," : "", slot, id);
        if (id < kFirstMutation) continue;
        char one[16];
        const int w = _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%u:%u", n ? "," : "", slot, id);
        if (w <= 0 || n + (size_t)w >= sizeof(out)) break;
        memcpy(out + n, one, (size_t)w + 1);
        n += (size_t)w;
    }
    strncpy_s(g.muts[c], out, _TRUNCATE);
    log_line("CLASSPICK", "  cat '%s' parts %s -- mutations: %s", g.name[c], all,
             out[0] ? out : "none");
}

bool safe_kind(void* item, int* out) {
    __try { *out = g.kind(item); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_locked(void* cat, int kind, bool* out) {
    __try { *out = g.locked(cat, kind); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_equip(void* cat, void* item, int list, bool* out) {
    __try { *out = g.equip(cat, item, list, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* inventory() {
    void* inv = nullptr;
    if (!g.base || !mem_read((const void*)(g.base + kRva_Inventory), &inv, sizeof(inv)))
        return nullptr;
    return inv;
}

// Walks one of the Inventory's lists. `fn` gets the Item and returns false to stop.
template <class F> void each_item(void* inv, uint8_t list, F fn) {
    uint8_t* head = nullptr;
    if (!mem_read((const uint8_t*)inv + kInv_List[list], &head, sizeof(head)) || !head) return;
    uint8_t* node = nullptr;
    if (!mem_read(head, &node, sizeof(node))) return;
    for (uint32_t guard = 0; node && node != head && guard < 4096; ++guard) {
        if (!fn(node + kNode_Item)) return;
        if (!mem_read(node, &node, sizeof(node))) return;
    }
}

// An item of the inventory: the one with this id, else one of the same key.
// Ids are handed out as a save loads and the two games' differ (2026-11-05: the
// friend's "Gobbler" was not the host's); two items of one key are alike.
uint8_t* find_item(uint64_t id, const char* key) {
    void* inv = inventory();
    if (!inv) return nullptr;
    uint8_t* by_key = nullptr;
    uint8_t* by_id  = nullptr;
    for (uint8_t list = 0; list < 2 && !by_id; ++list)
        each_item(inv, list, [&](uint8_t* item) {
            uint64_t have = 0;
            char k[48] = {};
            mem_read(item, &have, sizeof(have));
            mem_read_std_string(item + kItem_Key, k, sizeof(k));
            if (strcmp(k, key) != 0) return true;
            if (have == id) { by_id = item; return false; }
            if (!by_key) by_key = item;
            return true;
        });
    return by_id ? by_id : by_key;
}

struct Items {
    uint32_t n = 0;
    uint64_t id[kClassPickItems]       = {};
    char     name[kClassPickItems][48] = {};
    uint8_t  kind[kClassPickItems]     = {};
    uint8_t  list[kClassPickItems]     = {};
};

void read_items(Items& out) {
    void* inv = inventory();
    if (!inv) return;
    for (uint8_t list = 0; list < 2; ++list)
        each_item(inv, list, [&](uint8_t* item) {
            if (out.n >= kClassPickItems) return false;
            int kind = -1;
            if (!safe_kind(item, &kind) || kind < 0 || kind >= (int)kClassPickSlots) return true;
            const uint32_t i = out.n;
            if (!mem_read(item, &out.id[i], sizeof(out.id[i]))) return true;
            mem_read_std_string(item + kItem_Key, out.name[i], sizeof(out.name[i]));
            out.kind[i] = (uint8_t)kind;
            out.list[i] = list;
            ++out.n;
            return true;
        });
}

void slot_name(uint32_t cat, uint32_t kind, char* out, size_t cap) {
    out[0] = 0;
    void* data = run_cat(cat);
    if (!data) return;
    const uint8_t* slot = (const uint8_t*)data + kCat_Slots + kind * kSlot_Stride;
    uint64_t id = 0;
    if (!mem_read(slot, &id, sizeof(id)) || id == kFree) return;
    mem_read_std_string(slot + kItem_Key, out, cap);
}

void* cat_by_key(uint64_t key);

// HOST: the friend's cat a move of theirs names, and its name for the log.
void* friends_cat(uint64_t key, char* nm, size_t cap) {
    nm[0] = 0;
    void* cat = nullptr;
    if (g.phase != 2 && run_gear()) {
        cat = cat_by_key(key);
        if (cat && run_owner(cat) != 0) cat = nullptr;
        if (cat) read_wstring((const uint8_t*)cat + kCat_Name, nm, cap);
        return cat;
    }
    const int ci = cat_index(key);
    cat = ci >= 0 && g.owner[ci] ? run_cat((uint32_t)ci) : nullptr;
    if (cat) strncpy_s(nm, cap, g.name[ci], _TRUNCATE);
    return cat;
}

void apply_equip(const Equip& e) {
    char nm[48];
    void* cat = friends_cat(e.key, nm, sizeof(nm));
    if (!cat || e.list > 1 || e.kind >= kClassPickSlots) {
        log_line("CLASSPICK", "!! refused the other player's '%s' -- not their cat", e.name);
        return;
    }
    // In either list (the gear screen's own equip names the list the slot's old
    // item goes back to, not the one this came from), by id or by key.
    void* found = find_item(e.item, e.name);
    if (!found) {
        log_line("CLASSPICK", "the '%s' the other player wanted is gone -- someone took it", e.name);
        return;
    }
    int kind = -1;
    bool locked = true, ok = false;
    if (!safe_kind(found, &kind) || kind != e.kind ||
        !safe_locked(cat, kind, &locked) || locked) {
        log_line("CLASSPICK", "'%s' cannot go on '%s' -- the slot is locked or the item moved",
                 e.name, nm);
        return;
    }
    if (!safe_equip(cat, found, e.list, &ok) || !ok) {
        log_line("CLASSPICK", "!! the game would not put '%s' on '%s'", e.name, nm);
        return;
    }
    {
        ChoiceMsg m{};
        m.kind      = kChoiceGearOn;
        m.cat       = e.key;
        m.node_seed = e.item;
        m.aux       = e.list;
        strncpy_s(m.name, e.name, _TRUNCATE);
        net_send_choice(m);
    }
    char real[48] = {};
    read_wstring((const uint8_t*)cat + kCat_Name, real, sizeof(real));
    log_line("CLASSPICK", "<- the other player put '%s' on '%s' (slot %u; the game's cat '%s')",
             e.name, nm, (unsigned)kind, real);
    g.sent_hash = 0;
}

bool safe_unequip(void* cat, void* item, int list, bool* out) {
    __try { *out = g.unequip(cat, item, list, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void apply_unequip(const Equip& e) {
    char nm[48];
    void* cat = friends_cat(e.key, nm, sizeof(nm));
    if (!cat || e.kind >= kClassPickSlots || !g.unequip) {
        log_line("CLASSPICK", "!! refused taking an item off -- not the other player's cat");
        return;
    }
    uint8_t* slot = (uint8_t*)cat + kCat_Slots + e.kind * kSlot_Stride;
    uint64_t id = kFree;
    char key[48] = {};
    if (!mem_read(slot, &id, sizeof(id)) || id == kFree) return;   // nothing on
    mem_read_std_string(slot + kItem_Key, key, sizeof(key));
    bool locked = true, ok = false;
    if (!safe_locked(cat, e.kind, &locked) || locked) {
        log_line("CLASSPICK", "'%s' cannot come off '%s' -- the slot is locked", key, nm);
        return;
    }
    if (!safe_unequip(cat, slot, e.list, &ok) || !ok) {
        log_line("CLASSPICK", "!! the game would not take '%s' off '%s'", key, nm);
        return;
    }
    {
        ChoiceMsg m{};
        m.kind      = kChoiceGearOff;
        m.cat       = e.key;
        m.node_seed = id;
        m.aux       = e.list;
        strncpy_s(m.name, key, _TRUNCATE);
        net_send_choice(m);
    }
    log_line("CLASSPICK", "<- the other player took '%s' off '%s', into the storage", key,
             nm);
    g.sent_hash = 0;
}

void send(uint8_t kind, uint32_t index, uint32_t count, uint32_t aux, uint64_t cat,
          const char* name) {
    ChoiceMsg m{};
    m.kind  = kind;
    m.index = index;
    m.count = count;
    m.aux   = aux;
    m.cat   = cat;
    if (name) strncpy_s(m.name, name, _TRUNCATE);
    net_send_choice(m);
}

void publish(uint8_t phase) {
    void* tags[kClassPickTags];
    const uint32_t n = phase == 1 ? read_tags(g.chooser, tags) : 0;
    uint64_t holder[kClassPickTags] = {};
    char     cls[kClassPickTags][48] = {};
    uint64_t h = 0xCBF29CE484222325ull ^ g.cats ^ ((uint64_t)n << 32);
    for (uint32_t t = 0; t < n; ++t) {
        const uint64_t on = tag_cat(tags[t]);
        holder[t] = on == kFree ? 0 : on;
        mem_read_std_string((const uint8_t*)tags[t] + kTag_Class, cls[t], sizeof(cls[t]));
        h = (h ^ holder[t]) * 0x100000001B3ull;
        for (const char* s = cls[t]; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
    }
    static Items items;
    static char  slots[kClassPickCats][kClassPickSlots][48];
    items = Items{};
    if (phase == 2) read_items(items);
    for (uint32_t i = 0; i < items.n; ++i) {
        h = (h ^ items.id[i] ^ ((uint64_t)items.list[i] << 56)) * 0x100000001B3ull;
    }
    static int32_t stats[kClassPickCats][kClassPickStats];
    static int32_t bonuses[kClassPickCats][kClassPickStats];
    static bool    have[kClassPickCats];
    for (uint32_t c = 0; c < g.cats; ++c) {
        void* data = run_cat(c);
        int32_t bonus[kClassPickStats] = {};
        have[c] = data && mem_read((const uint8_t*)data + kCat_Stats, stats[c], sizeof(stats[c])) &&
                  mem_read((const uint8_t*)data + kCat_StatsBonus, bonus, sizeof(bonus));
        read_mutations(c, data);
        for (const char* s = g.muts[c]; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
        if (have[c])
            for (uint32_t s = 0; s < kClassPickStats; ++s) stats[c][s] += bonus[s];
        for (uint32_t s = 0; s < kClassPickStats; ++s) bonuses[c][s] = have[c] ? bonus[s] : 0;
        if (have[c])
            for (uint32_t s = 0; s < kClassPickStats; ++s)
                h = (h ^ (uint32_t)stats[c][s]) * 0x100000001B3ull;
    }
    for (uint32_t c = 0; c < g.cats; ++c)
        for (uint32_t k = 0; k < kClassPickSlots; ++k) {
            slots[c][k][0] = 0;
            if (phase == 2) slot_name(c, k, slots[c][k], sizeof(slots[c][k]));
            for (const char* s = slots[c][k]; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
            h = (h ^ (c * 8 + k)) * 0x100000001B3ull;
        }

    h = (h ^ phase) * 0x100000001B3ull;
    const uint64_t now = GetTickCount64();
    if (h == g.sent_hash && now - g.sent_at < kResendMs) return;
    for (uint32_t i = 0; i < g.cats; ++i)
        send(kChoiceClassCat, i, g.cats, g.owner[i], g.key[i], g.name[i]);
    for (uint32_t t = 0; t < n; ++t)
        send(kChoiceClassTag, t, n, 0, holder[t], cls[t]);
    for (uint32_t i = 0; i < items.n; ++i)
        send(kChoiceClassItem, i, items.n, items.kind[i] | (items.list[i] << 8), items.id[i],
             items.name[i]);
    for (uint32_t c = 0; c < g.cats; ++c)
        for (uint32_t k = 0; k < kClassPickSlots; ++k)
            send(kChoiceClassSlot, c, k, 0, g.key[c], slots[c][k]);
    for (uint32_t c = 0; c < g.cats; ++c) {
        if (!have[c]) continue;
        auto u16 = [&](uint32_t s) { return (uint64_t)(uint16_t)(int16_t)stats[c][s]; };
        ChoiceMsg m{};
        m.kind      = kChoiceClassStats;
        m.index     = c;
        m.cat       = g.key[c];
        m.node_seed = u16(0) | (u16(1) << 16) | (u16(2) << 32) | (u16(3) << 48);
        m.aux       = (uint32_t)(u16(4) | (u16(5) << 16));
        m.count     = (uint32_t)u16(6);
        net_send_choice(m);
        auto b16 = [&](uint32_t s) { return (uint64_t)(uint16_t)(int16_t)bonuses[c][s]; };
        ChoiceMsg bm = m;
        bm.kind      = kChoiceClassBonus;
        bm.node_seed = b16(0) | (b16(1) << 16) | (b16(2) << 32) | (b16(3) << 48);
        bm.aux       = (uint32_t)(b16(4) | (b16(5) << 16));
        bm.count     = (uint32_t)b16(6);
        net_send_choice(bm);
    }
    for (uint32_t c = 0; c < g.cats; ++c)
        if (g.muts[c][0]) send(kChoiceClassMuts, c, 0, 0, g.key[c], g.muts[c]);
    send(kChoiceClassEnd, n, g.cats, phase, 0, nullptr);
    if (!g.open_sent || g.phase != phase) {
        if (phase == 1)
            log_line("CLASSPICK", "the class chooser is up -- %u cat(s), %u collar(s), sent to"
                                  " the other player", g.cats, n);
        else
            log_line("CLASSPICK", "the gear screen is up -- %u cat(s), %u item(s), sent to"
                                  " the other player", g.cats, items.n);
    }
    g.open_sent = true;
    g.phase     = phase;
    g.sent_hash = h;
    g.sent_at   = now;
}

// CLIENT: a cat by its registry key.
void* cat_by_key(uint64_t key) {
    if (!g.lookup || !key) return nullptr;
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    void* reg = nullptr;
    void* cat = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir)) || !dir ||
        !mem_read((const uint8_t*)dir + kDir_Registry, &reg, sizeof(reg)) || !reg ||
        !safe_lookup(reg, key, &cat))
        return nullptr;
    return cat;
}

bool safe_unequip_to(void* cat, void* item, int list, bool* out) {
    __try { *out = g.unequip(cat, item, list, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// CLIENT: the host's gear moves, done here with the same items. From inside this
// game's gear screen update, as the host's own were.
bool apply_gear() {
    bool any = false;
    for (uint32_t i = 0; i < g.gear_n; ++i) {
        const State::Gear& e = g.gear[i];
        void* cat = cat_by_key(e.cat);
        if (!cat) { log_line("CLASSPICK", "!! no cat %016llx here for '%s'", (unsigned long long)e.cat, e.name); continue; }
        bool ok = false;
        if (e.on) {
            void* found = find_item(e.item, e.name);
            if (!found) { log_line("CLASSPICK", "!! '%s' is not in this game's inventory", e.name); continue; }
            if (!safe_equip(cat, found, e.list, &ok) || !ok)
                log_line("CLASSPICK", "!! the game would not put '%s' on here", e.name);
            else {
                any = true;
                log_line("CLASSPICK", "<- the host put '%s' on, as on its screen", e.name);
            }
        } else {
            // The slot wearing it: by id, else by key.
            uint8_t* slot = nullptr;
            uint8_t* by_key = nullptr;
            for (uint32_t k = 0; k < kClassPickSlots && !slot; ++k) {
                uint8_t* s = (uint8_t*)cat + kCat_Slots + k * kSlot_Stride;
                uint64_t id = kFree;
                char key[48] = {};
                if (!mem_read(s, &id, sizeof(id)) || id == kFree) continue;
                mem_read_std_string(s + kItem_Key, key, sizeof(key));
                if (strcmp(key, e.name) != 0) continue;
                if (id == e.item) slot = s;
                else if (!by_key) by_key = s;
            }
            if (!slot) slot = by_key;
            if (!slot) { log_line("CLASSPICK", "!! '%s' is not on that cat here", e.name); continue; }
            if (!safe_unequip_to(cat, slot, e.list, &ok) || !ok)
                log_line("CLASSPICK", "!! the game would not take '%s' off here", e.name);
            else {
                any = true;
                log_line("CLASSPICK", "<- the host took '%s' off, as on its screen", e.name);
            }
        }
    }
    g.gear_n = 0;
    // Moved here by the host: not this player's edit for the bag sync to send.
    if (any) follow_edit_rebase();
    return any;
}

void apply_equips() {
    for (uint32_t i = 0; i < g.equipped; ++i)
        g.equips[i].off ? apply_unequip(g.equips[i]) : apply_equip(g.equips[i]);
    g.equipped = 0;
}

void close_menu(const char* why) {
    if (g.open_sent) {
        send(kChoiceClassEnd, 0, 0, 0, 0, nullptr);
        log_line("CLASSPICK", "%s", why);
    }
    g.open_sent = false;
    g.sent_hash = 0;
    g.phase     = 0;
}

void forget_cats() {
    g.claimed_for = nullptr;
    g.cats        = 0;
    g.queued      = 0;
    g.equipped    = 0;
    g.chooser     = nullptr;
    g.gear_seen   = false;
    g.chooser_gone_at = 0;
}

// Two screens, as the game has them: the collars while the chooser is up, then
// the gear while the inventory screen it opens next is up. The cats the chooser
// handed out are kept between the two.
void host_pump() {
    const uint64_t now = GetTickCount64();
    // The owner table, for the client's backpack.
    static uint64_t owners_at = 0;
    if (now - owners_at > 2000 && net_active() && net_peer_count() >= 2 && in_a_run()) {
        owners_at = now;
        uint64_t ids[16];
        uint8_t  pos[16];
        const uint32_t n = lockstep_owners_copy(ids, pos, 16);
        for (uint32_t i = 0; i < n; ++i) {
            ChoiceMsg m{};
            m.kind = kChoiceRunOwner;
            m.cat  = ids[i];
            m.aux  = pos[i];
            net_send_choice(m);
        }
    }
    // The friend's backpack moves, with this game's backpack down (up, they go
    // from inside its update, see inv_tick).
    if (g.equipped && !inv_up() && g.phase != 2 && run_gear()) apply_equips();
    uint8_t phase = 0;
    // Not the backpack on the map, nor anything in a fight: those are the run's
    // own inventory, which the cat sync already carries.
    // Not the backpack opened on the run's map either: the map stops ticking
    // under it, and taken for the House defense's gear screen it locked the
    // friend's backpack shut (its confirm is this screen's close; 2026-11-07).
    if (!chooser_up() && !g.cats && inv_up() && !follow_on_map() && !combatlock_menu_live() &&
        !leave_scene_up("Map") && now - g.run_tried_at > kScanMs) {
        g.run_tried_at = now;
        if (claim_run()) g.chooser_gone_at = now;
    }
    if (chooser_up()) {
        phase = 1;
        g.chooser_gone_at = 0;
        g.gear_seen       = false;
    } else if (g.cats) {
        if (!g.chooser_gone_at) {
            g.chooser_gone_at = now;
            g.queued          = 0;
            close_menu("the class chooser closed -- the gear comes on the next screen");
        }
        if (inv_up()) {
            phase       = 2;
            g.gear_seen = true;
        } else if (g.gear_seen || now - g.chooser_gone_at > kGearWaitMs) {
            close_menu(g.gear_seen ? "the gear screen closed" : "no gear screen came up");
            forget_cats();
            return;
        }
    } else {
        if (g.open_sent) close_menu("the class chooser closed");
        g.chooser = nullptr;
        return;
    }
    if (!vote_on() || !phase) return;
    // See embark. Still up a moment later, the embark asked about a collarless
    // cat and was told no: the chooser goes on, and so does its list.
    if (phase == 1 && g.embarking == g.chooser) {
        if (now - g.embarking_at < 1500) return;
        g.embarking = nullptr;
    }
    if (phase == 1) {
        if (g.claimed_for != g.chooser) claim(g.chooser);
        if (!g.cats) return;
        for (uint32_t i = 0; i < g.queued; ++i) apply(g.queue[i]);
        g.queued = 0;
    }
    if (g.sent_hash && g.phase == phase && now - g.scanned_at < kScanMs) return;
    g.scanned_at = now;
    publish(phase);
}

} // namespace

// ---------------------------------------------------------------------------

void classpick_set_base(uintptr_t base) {
    if (g.o_tick) return;
    if (memcmp((const void*)(base + kRva_TagClick), kTagClickPrologue,
               sizeof(kTagClickPrologue)) != 0) {
        log_line("CLASSPICK", "!! the collar click moved -- the other player cannot pick collars");
        return;
    }
    g.click = (fn_click)(base + kRva_TagClick);
    if (memcmp((const void*)(base + kRva_PartList), kPartListPrologue, 8) == 0)
        g.parts = (fn_parts)(base + kRva_PartList);
    else
        log_line("CLASSPICK", "!! the cat's part list moved -- no mutations for the other player");
    if (memcmp((const void*)(base + kRva_CatLookup), kLookupPrologue, 8) == 0)
        g.lookup = (fn_lookup)(base + kRva_CatLookup);
    else
        log_line("CLASSPICK", "!! the cat lookup moved -- the other player's gear has no cat to go on");
    g.base  = base;
    if (memcmp((const void*)(base + kRva_Equip), kEquipPrologue, 8) == 0 &&
        memcmp((const void*)(base + kRva_ItemKind), kKindPrologue, 8) == 0 &&
        memcmp((const void*)(base + kRva_SlotLocked), kLockedPrologue, 8) == 0) {
        g.equip  = (fn_equip)(base + kRva_Equip);
        g.kind   = (fn_kind)(base + kRva_ItemKind);
        g.locked = (fn_locked)(base + kRva_SlotLocked);
        hook_slot_locked(base);
        if (memcmp((const void*)(base + kRva_Unequip), kUnequipPrologue, 8) == 0)
            g.unequip = (fn_unequip)(base + kRva_Unequip);
        else
            log_line("CLASSPICK", "!! the unequip call moved -- the other player cannot take items off");
        if (g.unequip) hook_gear(base);
    } else {
        log_line("CLASSPICK", "!! the equip calls moved -- the other player cannot pick items");
    }
    if (!patch(base, kRva_TagTickSlot, kRva_TagTick, (void*)&tag_tick, g.o_tick,
               "the collar box"))
        return;
    patch(base, kRva_TagClickSlot, kRva_TagClickCall, (void*)&tag_click, g.o_click,
          "the collar click");
    patch(base, kRva_EmbarkSlot, kRva_Embark, (void*)&embark, g.o_embark,
          "the embark button");
    patch(base, kRva_GearConfirmSlot, kRva_GearConfirm, (void*)&gear_confirm, g.o_confirm,
          "the gear screen's confirm");
    if (!patch(base, kRva_InvScreenSlot, kRva_InvScreenTick, (void*)&inv_tick, g.o_inv,
               "the inventory screen"))
        log_line("CLASSPICK", "!! so the other player cannot put gear on their cats");
}

// CLIENT, its own chooser: the host's cats' collars as the host has them. Only
// tags a host cat wears or wore here are touched; this player's own picks are
// theirs, and the host takes them as from the mod's window.
// The class a cat wears on this chooser, "" for none, and which tag.
int wears_here(void* const* tags, uint32_t n, uint64_t key, char* cls, size_t cap) {
    cls[0] = 0;
    for (uint32_t t = 0; t < n; ++t)
        if (tag_cat(tags[t]) == key) {
            mem_read_std_string((const uint8_t*)tags[t] + kTag_Class, cls, cap);
            return (int)t;
        }
    return -1;
}

// ...and on the host's, by its last list.
const char* wears_there(uint64_t key) {
    for (uint32_t t = 0; t < g.view.tags; ++t)
        if (g.view.tag_holder[t] == key) return g.view.tag_name[t];
    return "";
}

void mirror_host() {
    if (!client_chooser() || !g.active || g.view.phase != 1) return;
    const uint64_t now = GetTickCount64();
    if (now - g.mirrored_at < kScanMs) return;
    g.mirrored_at = now;
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
    // Only against a list of the host's chooser that is up: as it closes (the
    // host confirmed) the list empties, and read as "no collars" it took every
    // host cat's off here (2026-11-05).
    if (!g.view.tags || n != g.view.tags) return;
    for (uint32_t c = 0; c < g.view.cats; ++c) {
        const uint64_t key = g.view.cat_key[c];
        char here[48];
        const int worn = wears_here(tags, n, key, here, sizeof(here));
        const char* there = wears_there(key);
        if (strcmp(here, there) == 0) continue;
        if (g.view.cat_mine[c]) {
            // This player's cat: the host's screen has it otherwise -- say again.
            if (now - g.wear_sent_at > 1000) {
                g.wear_sent_at = now;
                send_wear(key);
            }
            continue;
        }
        // The host's cat: as on the host's screen. Off, or a free collar of its class.
        int t = -1;
        if (!there[0]) t = worn;
        else {
            for (uint32_t i = 0; i < n && t < 0; ++i) {
                char cls[48] = {};
                mem_read_std_string((const uint8_t*)tags[i] + kTag_Class, cls, sizeof(cls));
                if (strcmp(cls, there) == 0 && tag_cat(tags[i]) == kFree) t = (int)i;
            }
        }
        if (t < 0) continue;
        const uint64_t cur = current_cat(g.chooser);
        mem_write((uint8_t*)g.chooser + kCh_Current, &key, sizeof(key));
        g.applying = true;
        g.click(tags[t]);
        g.applying = false;
        mem_write((uint8_t*)g.chooser + kCh_Current, &cur, sizeof(cur));
        log_line("CLASSPICK", "<- the host's cat wears %s, as on the host's screen",
                 there[0] ? there : "no collar");
        return;   // one a look: the next look sees what it did
    }
}

// CLIENT: the host confirmed its collars, so this game's chooser confirms too and
// goes on to its own gear screen. The embark button's callback, <lambda_1> of
// ClassChooser::init (vftable 0xEE2250, _Do_call sub_140141E40), is
// {vftable, MewDirector*, ClassChooser*}; one is made here to call it with.
constexpr uintptr_t kRva_EmbarkLambdaVt = 0xEE2250;
void follow_embark() {
    // Once a chooser: while the collarless popup is up the chooser does not tick,
    // and taking that for its end confirmed it again and again (2026-11-05).
    if (g.embark_for != g.chooser) { g.embark_for = g.chooser; g.embark_due = false; }
    if (!client_chooser() || !g.active || g.view.phase != 2 || g.embark_due) return;
    void* const* slot = (void* const*)addr_of_data(D_MewDirectorPtr);
    void* dir = nullptr;
    if (!slot || !mem_read(slot, &dir, sizeof(dir)) || !dir) return;
    // Not on the stack: the popup's answer calls back through it later, and a
    // dead stack frame there is what the client fell over on "no".
    g.embark_closure[0] = (void*)(g.base + kRva_EmbarkLambdaVt);
    g.embark_closure[1] = dir;
    g.embark_closure[2] = g.chooser;
    g.embark_due = true;
    g.o_embark(g.embark_closure, nullptr, nullptr, nullptr);
    log_line("CLASSPICK", "<- the host confirmed its collars -- this game's chooser confirms too");
}

void classpick_pump() {
    if (!g.o_tick || !g.o_click) return;
    if (net_role() == NetRole::Host) { host_pump(); return; }
    if (g.gear_n && !inv_up() && run_gear()) apply_gear();
    mirror_host();
    if (g.active && GetTickCount64() - g.last_end > kStaleMs) {
        g.active = false;
        log_line("CLASSPICK", "the host stopped sending its class chooser");
    }
}

bool classpick_on_message(const ChoiceMsg& m) {
    if ((m.kind < kChoiceClassCat || m.kind > kChoiceClassEquip) &&
        m.kind != kChoiceClassStats && m.kind != kChoiceClassUnequip && m.kind != kChoiceClassMuts &&
        m.kind != kChoiceClassBonus && m.kind != kChoiceClassWear &&
        m.kind != kChoiceGearOn && m.kind != kChoiceGearOff && m.kind != kChoiceRunOwner)
        return false;
    const bool host = net_role() == NetRole::Host;

    if (m.kind == kChoiceRunOwner) {
        if (!host) lockstep_owner_put(m.cat, (uint8_t)m.aux);
        return true;
    }

    if (m.kind == kChoiceClassPick || m.kind == kChoiceClassWear) {
        if (!host) return true;
        // A newer state for the same cat replaces one not applied yet.
        Pick* p = nullptr;
        if (m.kind == kChoiceClassWear)
            for (uint32_t i = 0; i < g.queued; ++i)
                if (g.queue[i].state && g.queue[i].key == m.cat) p = &g.queue[i];
        if (!p && g.queued < 8) p = &g.queue[g.queued++];
        if (p) {
            p->key   = m.cat;
            p->tag   = m.index;
            p->state = m.kind == kChoiceClassWear;
            strncpy_s(p->name, m.name, _TRUNCATE);
        }
        return true;
    }
    if (m.kind == kChoiceClassUnequip) {
        if (!host || !g.unequip) return true;
        if (g.equipped < 8) {
            Equip& e = g.equips[g.equipped++];
            e = Equip{};
            e.key  = m.cat;
            e.kind = (uint8_t)(m.aux & 0xFF);
            // A list given (the client's own gear screen) is +1; the mod's window
            // gives none, and its items go to the storage.
            e.list = (m.aux >> 8) ? (uint8_t)((m.aux >> 8) - 1) : kUnequipList;
            e.off  = true;
        }
        return true;
    }
    if (m.kind == kChoiceClassEquip) {
        if (!host || !g.equip) return true;
        if (g.equipped < 8) {
            Equip& e = g.equips[g.equipped++];
            e = Equip{};
            e.key  = m.cat;
            e.item = m.node_seed;
            e.list = (uint8_t)(m.aux >> 8);
            e.kind = (uint8_t)(m.aux & 0xFF);
            strncpy_s(e.name, m.name, _TRUNCATE);
        }
        return true;
    }
    if (host) return true;

    ClassPickView& b = g.building;
    if (m.kind == kChoiceClassItem && m.index < kClassPickItems) {
        b.items = m.count < kClassPickItems ? m.count : kClassPickItems;
        b.item_id[m.index]   = m.cat;
        b.item_kind[m.index] = (uint8_t)(m.aux & 0xFF);
        b.item_list[m.index] = (uint8_t)(m.aux >> 8);
        strncpy_s(b.item_name[m.index], m.name, _TRUNCATE);
        return true;
    }
    if (m.kind == kChoiceClassStats) {
        if (m.index < kClassPickCats) {
            int16_t* s = b.stat[m.index];
            for (uint32_t i = 0; i < 4; ++i) s[i] = (int16_t)(m.node_seed >> (16 * i));
            s[4] = (int16_t)(m.aux & 0xFFFF);
            s[5] = (int16_t)(m.aux >> 16);
            s[6] = (int16_t)(m.count & 0xFFFF);
            b.have_stats[m.index] = true;
        }
        return true;
    }
    if (m.kind == kChoiceClassSlot) {
        if (m.index < kClassPickCats && m.count < kClassPickSlots)
            strncpy_s(b.slot_name[m.index][m.count], m.name, _TRUNCATE);
        return true;
    }
    if (m.kind == kChoiceGearOn || m.kind == kChoiceGearOff) {
        if (!host && g.gear_n < 64) {
            State::Gear& e = g.gear[g.gear_n++];
            e.on   = m.kind == kChoiceGearOn;
            e.cat  = m.cat;
            e.item = m.node_seed;
            e.list = (uint8_t)(m.aux & 0xFF);
            strncpy_s(e.name, m.name, _TRUNCATE);
        }
        return true;
    }
    if (m.kind == kChoiceClassBonus) {
        if (!host && m.index < kClassPickCats) {
            int16_t* s = b.bonus[m.index];
            for (uint32_t i = 0; i < 4; ++i) s[i] = (int16_t)(m.node_seed >> (16 * i));
            s[4] = (int16_t)(m.aux & 0xFFFF);
            s[5] = (int16_t)(m.aux >> 16);
            s[6] = (int16_t)(m.count & 0xFFFF);
        }
        return true;
    }
    if (m.kind == kChoiceClassMuts) {
        if (!host && m.index < kClassPickCats) strncpy_s(b.muts[m.index], m.name, _TRUNCATE);
        return true;
    }
    if (m.kind == kChoiceClassCat && m.index < kClassPickCats) {
        b.cats = m.count < kClassPickCats ? m.count : kClassPickCats;
        b.muts[m.index][0] = 0;
        memset(b.bonus[m.index], 0, sizeof(b.bonus[m.index]));
        b.cat_key[m.index]  = m.cat;
        b.cat_mine[m.index] = m.aux == net_peer_pos();
        strncpy_s(b.cat_name[m.index], m.name, _TRUNCATE);
    } else if (m.kind == kChoiceClassTag && m.index < kClassPickTags) {
        b.tags = m.count < kClassPickTags ? m.count : kClassPickTags;
        b.tag_holder[m.index] = m.cat;
        strncpy_s(b.tag_name[m.index], m.name, _TRUNCATE);
    } else if (m.kind == kChoiceClassEnd) {
        if (m.aux) {
            if (!g.active || g.view.phase != m.aux)
                log_line("CLASSPICK", m.aux == 1
                    ? "the host's class chooser is up -- pick your cats' collars in the panel"
                    : "the host's gear screen is up -- dress your cats in the panel");
            b.phase    = (uint8_t)m.aux;
            g.view     = b;
            g.active   = true;
            g.last_end = GetTickCount64();
        } else {
            if (g.active) log_line("CLASSPICK", g.view.phase == 2 ? "the host's gear screen closed"
                                                                  : "the host embarked");
            g.active = false;
        }
        b = ClassPickView{};
    }
    return true;
}

bool classpick_view(ClassPickView& out) {
    if (!g.active || net_role() == NetRole::Host) return false;
    // The collars are picked on this game's own screen when it has one.
    if (g.view.phase == 1 && chooser_up()) return false;
    if (g.view.phase == 2 && inv_up()) return false;
    out = g.view;
    return true;
}

void classpick_equip(uint64_t cat_key, uint32_t item) {
    if (!g.active || g.view.phase != 2 || item >= g.view.items) return;
    ChoiceMsg m{};
    m.kind      = kChoiceClassEquip;
    m.cat       = cat_key;
    m.node_seed = g.view.item_id[item];
    m.aux       = g.view.item_kind[item] | (g.view.item_list[item] << 8);
    strncpy_s(m.name, g.view.item_name[item], _TRUNCATE);
    net_send_choice(m);
    log_line("CLASSPICK", "-> '%s' for cat %016llx", m.name, (unsigned long long)cat_key);
}

void classpick_unequip(uint64_t cat_key, uint32_t kind) {
    if (!g.active || g.view.phase != 2 || kind >= kClassPickSlots) return;
    ChoiceMsg m{};
    m.kind = kChoiceClassUnequip;
    m.cat  = cat_key;
    m.aux  = kind;
    net_send_choice(m);
    log_line("CLASSPICK", "-> take slot %u off cat %016llx", kind, (unsigned long long)cat_key);
}

void classpick_pick(uint64_t cat_key, uint32_t tag) {
    if (!g.active || tag >= g.view.tags) return;
    send(kChoiceClassPick, tag, g.view.tags, 0, cat_key, g.view.tag_name[tag]);
    log_line("CLASSPICK", "-> the '%s' collar for cat %016llx", g.view.tag_name[tag],
             (unsigned long long)cat_key);
}

} // namespace mgmp
