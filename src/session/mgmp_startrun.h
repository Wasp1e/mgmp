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
void startrun_scene_tick();

// kChoiceRun*: true when the message was one of these.
bool startrun_on_message(const ChoiceMsg& m);

} // namespace mgmp
