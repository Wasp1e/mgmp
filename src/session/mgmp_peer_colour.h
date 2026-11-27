#pragma once

#include "mgmp_proto.h"

namespace mgmp {

inline constexpr float kPeerRGB[kMaxPeers][3] = {
    { 1.00f, 0.80f, 0.20f }, // host
    { 0.30f, 0.80f, 1.00f },
    { 0.45f, 1.00f, 0.45f },
    { 1.00f, 0.40f, 0.85f },
};

} // namespace mgmp
