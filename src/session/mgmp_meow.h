#pragma once
// A cat's meow, the way the main menu's meow button does it: a random voice
// (audio/voices/<male|female>N/) in a random mood (angry, happy, normal, sad),
// read out of this peer's resources.gpak. Played on the map on every click of
// a node by either player, so each hears the other clicking.

namespace mgmp {

// Returns at once; the file is read and played on a thread of its own.
void meow_play();

} // namespace mgmp
