#pragma once
#include <cstdint>
#include "mgmp_proto.h"

// THE FRIEND'S LOG, ON THE HOST'S DISK. A desync is two logs read side by
// side, and the second one lived on the other player's machine until they
// found it and sent it by hand (2026-11-05: "for debugging"). Now the client
// sends the end of its log to the host when something goes wrong, and the host
// keeps it in logs\friend\ beside its own.

namespace mgmp {

// Something went wrong here or at the other end. The client sends its log; the
// host asks the client for it. Rate-limited, so a burst of errors sends one.
void peerlog_trouble(const char* why);

// The panel's button: the same, now, whatever the rate limit says.
void peerlog_ask(const char* why);

// Host: a client's log arrived.
void peerlog_on_message(const LogFileMsg& m, uint8_t from);

// Client: the host asked for this peer's log (kChoiceLogRequest).
bool peerlog_on_choice(const ChoiceMsg& m);

// Where the last log from the friend was written, empty when none yet.
const char* peerlog_last_saved();

} // namespace mgmp
