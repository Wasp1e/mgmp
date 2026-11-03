#pragma once
// THE GAME'S OWN WORDS FOR AN ITEM, read from this peer's resources.gpak: the
// item's gon entry (data/items/*.gon -- name, desc, its stat bonuses) and the
// Russian column of data/text/combined.csv. Loaded once, on first use.
#include <cstddef>

namespace mgmp {

struct ItemText {
    char name[96]   = {};   // "Осколок стекла"; the key itself when there is no text
    char desc[512]  = {};   // markup like [img:str] taken out
    char bonus[192] = {};   // "Сила +1, Щит +4", empty when none
    char icon[32]   = {};   // "weapon_2": icons/<kind>_<frame>.png, empty when none
};

// False when the key is no item this game knows.
bool gametext_item(const char* key, ItemText& out);

} // namespace mgmp
