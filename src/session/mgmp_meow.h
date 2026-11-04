#pragma once
#include <cstdint>
// A cat's meow, the way the main menu's meow button does it: a random voice
// (audio/voices/<male|female>N/) in a random mood (angry, happy, normal, sad),
// read out of this peer's resources.gpak. Played on the map on every click of
// a node by either player, so each hears the other clicking.

namespace mgmp {

// A new meow for a click: a number the clicking peer draws and sends with its
// vote. Never 0.
uint32_t meow_new_seed();

// Plays the meow `seed` names. The same seed is the same voice, mood and take
// on every machine -- the voices come from each peer's own copy of the same
// game files -- so both players hear the one meow. 0 plays a random one.
// Returns at once; the file is read and played on a thread of its own.
void meow_play(uint32_t seed);

} // namespace mgmp
