#pragma once
#include <cstdint>
#include <cstring>

namespace mgmp {
// The direct Face call in Brain::UpdateDecision. Match the argument setup
// AND the relative call destination; status/action calls inside the same
// update must still run. `bytes` are the 22 bytes preceding the return address.
inline bool preview_face_call(const uint8_t* bytes, uintptr_t return_address,
                              uintptr_t face) {
    static constexpr uint8_t setup[] = {
        0x45,0x33,0xC9, 0x45,0x33,0xC0,
        0x48,0x8B,0x97,0x38,0x02,0x00,0x00,
        0x48,0x8B,0x4F,0x38, 0xE8
    };
    if (std::memcmp(bytes, setup, sizeof(setup))) return false;
    int32_t displacement = 0;
    std::memcpy(&displacement, bytes + sizeof(setup), sizeof(displacement));
    return return_address + displacement == face;
}
} // namespace mgmp
