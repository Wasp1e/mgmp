#pragma once
#include <cstdint>
#include "mgmp_proto.h"

// WHAT HAPPENED IN THE NIGHT. The day ends on the host: its House resolves the
// night (kittens, fights, injuries, old age...) and shows each event as an
// Interstitial, and the client only gets the morning's save and reloads into
// it, none the wiser (2026-11-05). So the host sends each event's text as it
// comes up, names filled in, and the client shows them all in one window once
// its reload is done.

namespace mgmp {

void night_set_base(uintptr_t base);

// Client: one line of the host's night arrived.
void night_on_message(const LogFileMsg& m);

// Client: the lines to show now, if any. `n` gets their count.
const char* const* night_view(uint32_t* n);

// Client: the player closed the window.
void night_dismiss();

// Client: the end-day sign was just pressed here and refused (for a note).
bool night_end_day_refused();

} // namespace mgmp
