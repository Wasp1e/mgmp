#include "mgmp_classpick.h"
#include "mgmp_lockstep.h"
#include "mgmp_catsync.h"
#include "mgmp_invsync.h"
#include "mgmp_net.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
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
constexpr uintptr_t kCat_StatsBonus = 0x70C;

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

struct Pick { uint64_t key; uint32_t tag; char name[48]; };
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

    // client
    ClassPickView view, building;
    bool     active   = false;
    uint64_t last_end = 0;
};

State g;

bool chooser_up() { return g.chooser && GetTickCount64() - g.seen < kGoneMs; }

void* __fastcall tag_tick(void* a, void* b, void* c, void* d) {
    void* ch = nullptr;
    if (mem_read((const uint8_t*)a + kTag_Chooser, &ch, sizeof(ch)) && ch) {
        g.chooser = ch;
        g.seen    = GetTickCount64();
    }
    return g.o_tick(a, b, c, d);
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

// The host's own click on a collar. The friend's cats are the friend's to dress.
void* __fastcall tag_click(void* a, void* b, void* c, void* d) {
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
void* __fastcall embark(void* a, void* b, void* c, void* d) {
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

void apply(const Pick& p) {
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
    void* data = catsync_cat_by_uid(g.uid[cat]);
    if (!data) return;
    const uint8_t* slot = (const uint8_t*)data + kCat_Slots + kind * kSlot_Stride;
    uint64_t id = 0;
    if (!mem_read(slot, &id, sizeof(id)) || id == kFree) return;
    mem_read_std_string(slot + kItem_Key, out, cap);
}

void apply_equip(const Equip& e) {
    const int ci = cat_index(e.key);
    void* cat = ci >= 0 ? catsync_cat_by_uid(g.uid[ci]) : nullptr;
    if (ci < 0 || !g.owner[ci] || !cat || e.list > 1 || e.kind >= kClassPickSlots) {
        log_line("CLASSPICK", "!! refused the other player's '%s' -- not their cat", e.name);
        return;
    }
    void* found = nullptr;
    if (void* inv = inventory())
        each_item(inv, e.list, [&](uint8_t* item) {
            uint64_t id = 0;
            char key[48] = {};
            if (mem_read(item, &id, sizeof(id)) && id == e.item &&
                mem_read_std_string(item + kItem_Key, key, sizeof(key)) &&
                strcmp(key, e.name) == 0) {
                found = item;
                return false;
            }
            return true;
        });
    if (!found) {
        log_line("CLASSPICK", "the '%s' the other player wanted is gone -- someone took it", e.name);
        return;
    }
    int kind = -1;
    bool locked = true, ok = false;
    if (!safe_kind(found, &kind) || kind != e.kind ||
        !safe_locked(cat, kind, &locked) || locked) {
        log_line("CLASSPICK", "'%s' cannot go on '%s' -- the slot is locked or the item moved",
                 e.name, g.name[ci]);
        return;
    }
    if (!safe_equip(cat, found, e.list, &ok) || !ok) {
        log_line("CLASSPICK", "!! the game would not put '%s' on '%s'", e.name, g.name[ci]);
        return;
    }
    log_line("CLASSPICK", "<- the other player put '%s' on '%s' (slot %u)", e.name,
             g.name[ci], (unsigned)kind);
    g.sent_hash = 0;
}

bool safe_unequip(void* cat, void* item, bool* out) {
    __try { *out = g.unequip(cat, item, kUnequipList, 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void apply_unequip(const Equip& e) {
    const int ci = cat_index(e.key);
    void* cat = ci >= 0 ? catsync_cat_by_uid(g.uid[ci]) : nullptr;
    if (ci < 0 || !g.owner[ci] || !cat || e.kind >= kClassPickSlots || !g.unequip) {
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
        log_line("CLASSPICK", "'%s' cannot come off '%s' -- the slot is locked", key, g.name[ci]);
        return;
    }
    if (!safe_unequip(cat, slot, &ok) || !ok) {
        log_line("CLASSPICK", "!! the game would not take '%s' off '%s'", key, g.name[ci]);
        return;
    }
    log_line("CLASSPICK", "<- the other player took '%s' off '%s', into the storage", key,
             g.name[ci]);
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

void publish() {
    void* tags[kClassPickTags];
    const uint32_t n = read_tags(g.chooser, tags);
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
    read_items(items);
    for (uint32_t i = 0; i < items.n; ++i) {
        h = (h ^ items.id[i] ^ ((uint64_t)items.list[i] << 56)) * 0x100000001B3ull;
    }
    static int32_t stats[kClassPickCats][kClassPickStats];
    static bool    have[kClassPickCats];
    for (uint32_t c = 0; c < g.cats; ++c) {
        void* data = catsync_cat_by_uid(g.uid[c]);
        int32_t bonus[kClassPickStats] = {};
        have[c] = data && mem_read((const uint8_t*)data + kCat_Stats, stats[c], sizeof(stats[c])) &&
                  mem_read((const uint8_t*)data + kCat_StatsBonus, bonus, sizeof(bonus));
        if (have[c])
            for (uint32_t s = 0; s < kClassPickStats; ++s) stats[c][s] += bonus[s];
        if (have[c])
            for (uint32_t s = 0; s < kClassPickStats; ++s)
                h = (h ^ (uint32_t)stats[c][s]) * 0x100000001B3ull;
    }
    for (uint32_t c = 0; c < g.cats; ++c)
        for (uint32_t k = 0; k < kClassPickSlots; ++k) {
            slot_name(c, k, slots[c][k], sizeof(slots[c][k]));
            for (const char* s = slots[c][k]; *s; ++s) h = (h ^ (uint8_t)*s) * 0x100000001B3ull;
            h = (h ^ (c * 8 + k)) * 0x100000001B3ull;
        }

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
    }
    send(kChoiceClassEnd, n, g.cats, 1, 0, nullptr);
    if (!g.open_sent)
        log_line("CLASSPICK", "the class chooser is up -- %u cat(s), %u collar(s), sent to"
                              " the other player", g.cats, n);
    g.open_sent = true;
    g.sent_hash = h;
    g.sent_at   = now;
}

void host_pump() {
    if (!chooser_up()) {
        if (g.open_sent) {
            send(kChoiceClassEnd, 0, 0, 0, 0, nullptr);
            log_line("CLASSPICK", "the class chooser closed");
        }
        g.open_sent   = false;
        g.claimed_for = nullptr;
        g.cats        = 0;
        g.queued      = 0;
        g.equipped    = 0;
        g.sent_hash   = 0;
        g.chooser     = nullptr;
        return;
    }
    if (!vote_on()) return;
    if (g.claimed_for != g.chooser) claim(g.chooser);
    if (!g.cats) return;
    for (uint32_t i = 0; i < g.queued; ++i) apply(g.queue[i]);
    g.queued = 0;
    // Not under the host's own inventory screen: it holds the lists this
    // changes, and two hands on one bag is how items doubled on the map.
    if (g.equipped) {
        if (invsync_screen_open()) {
            if (!g.said_screen)
                log_line("CLASSPICK", "the other player's item waits for your inventory to close");
            g.said_screen = true;
        } else {
            for (uint32_t i = 0; i < g.equipped; ++i)
                g.equips[i].off ? apply_unequip(g.equips[i]) : apply_equip(g.equips[i]);
            g.equipped    = 0;
            g.said_screen = false;
        }
    }
    const uint64_t now = GetTickCount64();
    if (g.sent_hash && now - g.scanned_at < kScanMs) return;
    g.scanned_at = now;
    publish();
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
    g.base  = base;
    if (memcmp((const void*)(base + kRva_Equip), kEquipPrologue, 8) == 0 &&
        memcmp((const void*)(base + kRva_ItemKind), kKindPrologue, 8) == 0 &&
        memcmp((const void*)(base + kRva_SlotLocked), kLockedPrologue, 8) == 0) {
        g.equip  = (fn_equip)(base + kRva_Equip);
        g.kind   = (fn_kind)(base + kRva_ItemKind);
        g.locked = (fn_locked)(base + kRva_SlotLocked);
        if (memcmp((const void*)(base + kRva_Unequip), kUnequipPrologue, 8) == 0)
            g.unequip = (fn_unequip)(base + kRva_Unequip);
        else
            log_line("CLASSPICK", "!! the unequip call moved -- the other player cannot take items off");
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
}

void classpick_pump() {
    if (!g.o_tick || !g.o_click) return;
    if (net_role() == NetRole::Host) { host_pump(); return; }
    if (g.active && GetTickCount64() - g.last_end > kStaleMs) {
        g.active = false;
        log_line("CLASSPICK", "the host stopped sending its class chooser");
    }
}

bool classpick_on_message(const ChoiceMsg& m) {
    if ((m.kind < kChoiceClassCat || m.kind > kChoiceClassEquip) &&
        m.kind != kChoiceClassStats && m.kind != kChoiceClassUnequip)
        return false;
    const bool host = net_role() == NetRole::Host;

    if (m.kind == kChoiceClassPick) {
        if (!host) return true;
        if (g.queued < 8) {
            Pick& p = g.queue[g.queued++];
            p.key = m.cat;
            p.tag = m.index;
            strncpy_s(p.name, m.name, _TRUNCATE);
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
    if (m.kind == kChoiceClassCat && m.index < kClassPickCats) {
        b.cats = m.count < kClassPickCats ? m.count : kClassPickCats;
        b.cat_key[m.index]  = m.cat;
        b.cat_mine[m.index] = m.aux == net_peer_pos();
        strncpy_s(b.cat_name[m.index], m.name, _TRUNCATE);
    } else if (m.kind == kChoiceClassTag && m.index < kClassPickTags) {
        b.tags = m.count < kClassPickTags ? m.count : kClassPickTags;
        b.tag_holder[m.index] = m.cat;
        strncpy_s(b.tag_name[m.index], m.name, _TRUNCATE);
    } else if (m.kind == kChoiceClassEnd) {
        if (m.aux) {
            if (!g.active)
                log_line("CLASSPICK", "the host's class chooser is up -- pick your cats'"
                                      " collars in the panel");
            g.view     = b;
            g.active   = true;
            g.last_end = GetTickCount64();
        } else {
            if (g.active) log_line("CLASSPICK", "the host embarked");
            g.active = false;
        }
        b = ClassPickView{};
    }
    return true;
}

bool classpick_view(ClassPickView& out) {
    if (!g.active || net_role() == NetRole::Host) return false;
    out = g.view;
    return true;
}

void classpick_equip(uint64_t cat_key, uint32_t item) {
    if (!g.active || item >= g.view.items) return;
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
    if (!g.active || kind >= kClassPickSlots) return;
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
