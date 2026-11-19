#pragma once
// mgmp_house -- THE HOUSE CHECK. Is the friend's House the host's?
//
// The night is resolved on the shared simulation stream (sub_1401E8B40 reads
// TLS+0x178 throughout), so a client whose House is the host's could resolve
// the same night itself, scenes and all, with no reload (the user's plan,
// 2026-11-15). That holds only if the Houses are the same, and during the day
// they are not kept so: the host's save reaches the client only when the day
// changes. So first, the check.
//
// WHAT IS COMPARED: the save itself. The House writes its whole state to the
// .sav (sqlite) as it changes -- a row per cat (`cats`, by key), per piece of
// furniture (`furniture`), the named blobs (`files`: house_state, pedigree,
// inventories...) and the scalars (`properties`: house_food, house_gold,
// current_day, random_seed...). When the host ends the day, each game reads
// its own file: the host sends a hash per row, the client compares with its
// own and logs every row that differs, and tells the host the verdict.
// It reports; it changes nothing.

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

// Host: the end-day sign started the night (the House's +0xB0 went up).
// `stream` is the simulation stream as the sign was pressed.
void house_night_starts(const uint64_t stream[4]);

// Either side: is this game's night over -- its event screens all closed and
// its save no longer being written? True when no night is under way. The
// morning is sent (host) and compared (client) only then: sent at the first
// write of the new day, it was half written, and the friend's furniture
// counter fell one behind for good (2026-11-15).
bool house_night_settled();

// Either side: a night event's screen is being made (InterstitialScene::init).
void house_on_scene(void* scene);

// Client: this game is resolving the night itself (from the host's stream),
// and the host's morning save has not been settled yet.
bool house_local_night();

// Client: the host's morning save, while this game resolved its own night.
// 0 wait, 1 this game's morning is the host's (keep it, no reload), 2 load it.
int house_morning(const uint8_t* data, uint32_t size);

// Host: the day-end check (inside house_night_starts).
void house_on_end_day();

// Hooks: the House's cat drop. Called once with the module base.
void house_set_base(uintptr_t base);

// From inside a House component's update (the NPC drawer's): the other
// player's moves are applied here. `component` is that component.
void house_tick(void* component);

// Either side: the other player moved a cat (kChoiceHouseMove).
bool house_on_choice(const ChoiceMsg& m, uint8_t from);

// Experimental remote drags must not be auto-collected by the local box.
bool house_drag_remote_held(void* cat);
void house_drag_remote_placed(void* cat);

// A remote pipe delivery must finish before the client's NPC office opens:
// otherwise the office can start a new quest before the previous cat counts.
bool house_pipe_delivery_pending();

// Either side: the note for the top right corner, or null -- which cat the
// other player sent down the pipe and to which NPC, for a few seconds after.
// From the draw (the same thread as the House's update).
const char* house_note();

// Either side: a House message arrived (MSG_NIGHT with reason "house"/"house_verdict").
// Returns false if the message is not one of these.
bool house_on_message(const LogFileMsg& m);

} // namespace mgmp
