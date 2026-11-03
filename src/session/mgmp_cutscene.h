#pragma once
// CUTSCENES ADVANCE ON THE HOST'S CLICK. While one is up, the host's left click,
// Space and Enter go to the client, the client's own are swallowed, and the
// host's are posted to its window as if they were its own -- so the dialog moves
// on both screens when the host clicks.
#include <cstdint>

namespace mgmp {

struct ChoiceMsg;

// Stamps SwfCutscene and DialogController updates, which is how a cutscene is seen.
void cutscene_set_base(uintptr_t base);

// Once a frame: logs a cutscene starting and ending, posts the host's inputs.
void cutscene_pump();

// kChoiceCutInput.
bool cutscene_on_message(const ChoiceMsg& m);

// From the window procedure, for a click or Space/Enter the game is about to get.
// True: swallow it.
bool cutscene_filter_input(unsigned msg, uintptr_t wparam);

} // namespace mgmp
