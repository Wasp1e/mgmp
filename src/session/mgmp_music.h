#pragma once

#include <cstdint>

namespace mgmp {

// The mixer hook only touches the game's music bus. Receive runs on the
// transport thread, and pump runs on the game thread after the handshake.
void music_set_base(uintptr_t base);
void music_reset();
void music_pump();
void music_receive(const uint8_t* data, uint32_t size, uint8_t from, bool host);

} // namespace mgmp
