#include "mgmp_previewface.h"
#include <cstdio>

int main() {
    // Real argument sequence with a negative rel32 (as in the shipped PE).
    uint8_t call[] = {0x45,0x33,0xC9,0x45,0x33,0xC0,0x48,0x8B,0x97,
        0x38,0x02,0,0,0x48,0x8B,0x4F,0x38,0xE8,0x62,0x4C,0xFD,0xFF};
    int failed = 0;
    auto check = [&](bool value, const char* name) {
        if (!value) { std::printf("FAIL: %s\n", name); ++failed; }
    };
    check(mgmp::preview_face_call(call, 0x1401381EEull, 0x14010CE50ull),
          "recognizes direct preview call");
    check(mgmp::preview_face_call(call, 0x7FF6131381EEull, 0x7FF61310CE50ull),
          "recognizes rebased game");
    check(!mgmp::preview_face_call(call, 0x1401381EEull, 0x14010CE51ull),
          "different callee passes through");
    call[9] = 0x30;
    check(!mgmp::preview_face_call(call, 0x1401381EEull, 0x14010CE50ull),
          "different direction source passes through");
    call[9] = 0x38;
    call[2] = 0xC0;
    check(!mgmp::preview_face_call(call, 0x1401381EEull, 0x14010CE50ull),
          "different flags pass through");
    return failed ? 1 : 0;
}
