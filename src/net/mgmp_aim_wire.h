#pragma once
#include "mgmp_proto.h"

namespace mgmp {

// PlayerBrain can select a targetless ability at an off-grid sentinel:
// MoonHandFlail uses (-4000, -4000). Sending that cosmetic preview unchanged
// fails dec_aim on an existing peer and disconnects it before ACTION arrives.
// Clear the old preview instead. Never change the actual gameplay decision.
inline void aim_sanitize_for_wire(AimMsg& m) {
    if (m.active &&
        m.tx >= -1000 && m.tx <= 1000 && m.ty >= -1000 && m.ty <= 1000 &&
        m.dx >= -8 && m.dx <= 8 && m.dy >= -8 && m.dy <= 8)
        return;
    const uint64_t battle = m.battle_id;
    const uint8_t cat = m.cat;
    m = AimMsg{};
    m.battle_id = battle;
    m.cat = cat;
}

} // namespace mgmp
