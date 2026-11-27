#include "mgmp_spriteface.h"
#include "mgmp_mem.h"
#include <cstring>
#include <cstdio>

namespace mgmp {
bool refresh_sprite_facing(void* character, SpriteFaceFn face, bool idle_only) {
    if (!character || !face) return false;
    auto* ch = (uint8_t*)character;
    uint8_t enabled = 0, dead = 1, modes[2]{}, rotated = 1, fixed_flip = 1;
    int32_t facing[4]{}, locked[2]{}, scale[2]{}, flip = 0;
    uint8_t* anim = nullptr;
    if (!mem_read(ch + 0x11, &enabled, 1) || !enabled ||
        !mem_read(ch + 0x4C2, &dead, 1) || dead ||
        !mem_read(ch + 0x4CC, modes, 2) || modes[0] || modes[1] ||
        !mem_read(ch + 0x3B4, &rotated, 1) || rotated ||
        !mem_read(ch + 0x3A8, &fixed_flip, 1) ||
        !mem_read(ch + 0x3AC, locked, sizeof(locked)) || locked[0] || locked[1] ||
        !mem_read(ch + 0x388, facing, sizeof(facing)) ||
        !mem_read(ch + 0x3A0, scale, sizeof(scale)) ||
        !mem_read(ch + 0x58, &anim, sizeof(anim)) || !anim) return false;
    const int x = facing[0], y = facing[1];
    if (!((x == 0 && (y == -1 || y == 1)) ||
          (y == 0 && (x == -1 || x == 1))) ||
        (scale[0] != -1 && scale[0] != 1) ||
        (scale[1] != -1 && scale[1] != 1)) return false;
    uint8_t turning = 1, busy = 1;
    if (!mem_read(anim + 0x210, &turning, 1) || turning ||
        !mem_read(anim + 0x2A0, &busy, 1) || busy ||
        !mem_read(anim + 0x14C, &flip, sizeof(flip))) return false;
    if (idle_only) {
        // Animator's active pose label, observed as "idle" after its queued
        // movement/turn animation finishes. Never interrupt walk/attack poses.
        char pose[64]{};
        if (!mem_read_std_string(anim + 0x108, pose, sizeof(pose)) ||
            std::strcmp(pose, "idle")) return false;
    }

    // Face's ordinary sprite branch at RVA 0x10DAFB. Use the committed
    // cardinal direction, with the character's own art axis/scale settings.
    const int dx = x * scale[1], dy = y * scale[1];
    const int expected_flip = (dx < 0 || dy > 0) ? -scale[0] : scale[0];
    char prefix[128]{}, suffix[160]{}, expected[160]{};
    if (!mem_read_std_string(ch + 0x3E0, prefix, sizeof(prefix)) ||
        !mem_read_std_string(anim + 0xB8, suffix, sizeof(suffix))) return false;
    std::snprintf(expected, sizeof(expected), "%s%c", prefix,
                  (dx > 0 || dy > 0) ? 'B' : 'F');
    if ((!fixed_flip && flip != expected_flip) || std::strcmp(suffix, expected)) {
        uint64_t dir = 0;
        std::memcpy(&dir, facing, sizeof(dir));
        // r8=0 avoids _DEFAULT resets; r9=0 and +4CC=0 skip turn
        // animations/callbacks. No statuses or damage run in this branch.
        face(character, dir, 0, 0);
        // Face writes the desired direction even when no action is committed.
        // Preserve both fields, including a pending diagonal/zero target.
        mem_write(ch + 0x388, facing, sizeof(facing));
        return true;
    }
    return false;
}
}
