#pragma once
#include <cstdint>
#include "mgmp_proto.h"

// THE CLIENT'S OWN CLASS CHOOSER. When the host sets off on an adventure, the
// client's game sets off too, into its own ClassChooser -- the game's screen,
// with the host's cats and the host's collars -- instead of the mod's window
// (2026-11-05, the user's ask). The host's run is still the one played: the
// client's embark does nothing, and the host's save follows as before.
//
//   MewDirector::GoOnAnAdventure  sub_1403B13F0(dir, std::string map,
//                                 vector<string> collars, cats by key): the
//                                 House's box of cats confirmed; it loads the
//                                 ClassChooser (the gear and the map come after).

namespace mgmp {

void startrun_set_base(uintptr_t base);

// Inside a House component's update: where the client's StartAdventure runs.
void startrun_house_tick();

// Inside the client's chooser or gear screen update: where the host's embark
// cutscene is played here (sub_1403C0210).
void startrun_scene_tick(void* gear_screen);   // the InventoryScreen2, or null

// CLIENT: 1 while this game is setting off to a map of its own from the host's
// stream, 2 once that map came out as the host's (the host's fresh save is not
// needed then), 0 otherwise. startrun_map_kept: the 2 was used.
int  startrun_map_state();

// CLIENT, inside its gear screen's update: the host's gear screen closed. If
// this one leaves without a cutscene of its own (to ActSelection or the map),
// it leaves now, as the host's did.
void startrun_gear_closed(void* gear_screen);
void startrun_map_kept();

// HOST: the position of the player who put the cat with this run-list key
// (CatData+0xC48) in the House's box, or -1. Such a cat is theirs for the run.
int startrun_box_picker(uint64_t key);

// The box up with a cat in it, and this player has a share to count (anyone
// but the host of two): how many of the cats in it this player put there, and
// the most they may.
bool startrun_box_note(uint32_t& mine, uint32_t& most);
bool startrun_box_drag_guard_ready();

// kChoiceRun*: true when the message was one of these.
bool startrun_on_message(const ChoiceMsg& m);

} // namespace mgmp
