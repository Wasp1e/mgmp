#pragma once
// THE CLASS CHOOSER, SHARED. When a run starts with fresh cats the host's game
// opens glaiel::ClassChooser: one collar (ClassTagBox) per class, put on the
// cat the screen is showing. Those cats are made on the host alone, so the
// client has no such screen; it gets a list of its own cats and the collars in
// the mod's panel instead, and its clicks are made on the host's screen.
//
// Which cats are whose is decided here, by the same rule and the same owner
// table the battles use, and written into that table, so the first battle
// hands each player the cats they dressed.
#include <cstdint>

namespace mgmp {

struct ChoiceMsg;

// Patches ClassTagBox's per-frame slot and its click, and the embark button.
void classpick_set_base(uintptr_t base);

// Once a frame, both roles. Host: publishes the chooser while it is up and
// makes the client's picks on it. Client: drops a menu the host stopped sending.
void classpick_pump();

// Choice kinds kChoiceClassCat..kChoiceClassEquip, kChoiceClassStats, kChoiceClassUnequip.
bool classpick_on_message(const ChoiceMsg& m);

// --- the client's menu, for the panel ----------------------------------------

constexpr uint32_t kClassPickCats  = 8;
constexpr uint32_t kClassPickTags  = 16;
constexpr uint32_t kClassPickItems = 128;
constexpr uint32_t kClassPickSlots = 5;   // head, face, neck, weapon, trinket
constexpr uint32_t kClassPickStats = 7;   // str, dex, con, int, spd, cha, lck

struct ClassPickView {
    // Which of the host's two screens: 1 the collars (ClassChooser), 2 the gear
    // (the inventory screen the game opens next). Items only on the second.
    uint8_t  phase = 1;
    uint32_t cats = 0;
    uint64_t cat_key[kClassPickCats]      = {};
    char     cat_name[kClassPickCats][48] = {};
    bool     cat_mine[kClassPickCats]     = {};
    uint32_t tags = 0;
    uint64_t tag_holder[kClassPickTags]   = {};   // a cat key, 0 when free
    char     tag_name[kClassPickTags][48] = {};
    uint32_t items = 0;
    uint64_t item_id[kClassPickItems]       = {};
    char     item_name[kClassPickItems][48] = {};
    uint8_t  item_kind[kClassPickItems]     = {};   // the slot it goes in
    uint8_t  item_list[kClassPickItems]     = {};   // 0 the bag, 1 the storage
    char     slot_name[kClassPickCats][kClassPickSlots][48] = {};
    int16_t  stat[kClassPickCats][kClassPickStats] = {};
    bool     have_stats[kClassPickCats] = {};
    char     muts[kClassPickCats][48]   = {};   // "slot:id,..."; see kChoiceClassMuts
    int16_t  bonus[kClassPickCats][kClassPickStats] = {};   // part of stat[], shown beside it
};

// True while the host's chooser is up and this peer is the client.
bool classpick_view(ClassPickView& out);

// CLIENT: this game's own class chooser or the gear screen after it is up. The
// chooser's embark writes its copies of the cats back over them, and the gear
// screen holds their items -- a cat applied under either is lost or crashes.
bool classpick_client_chooser_up();

// The client puts collar `tag` on its cat `cat_key`.
void classpick_pick(uint64_t cat_key, uint32_t tag);

// The client puts item `item` of the menu on its cat `cat_key`.
void classpick_equip(uint64_t cat_key, uint32_t item);

// The client takes what its cat wears in slot `kind` off.
void classpick_unequip(uint64_t cat_key, uint32_t kind);

} // namespace mgmp
