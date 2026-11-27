#pragma once
#include <cstdint>
namespace mgmp {
using SpriteFaceFn = void (__fastcall*)(void*, uint64_t, unsigned char, unsigned char);
// Only the ordinary, non-animated Face branch. Caller supplies a verified
// native function and a live Character at a turn boundary. Returns true only
// when a stale sprite was refreshed. Combat facing/target are preserved.
bool refresh_sprite_facing(void* character, SpriteFaceFn face, bool idle_only = false);
}
