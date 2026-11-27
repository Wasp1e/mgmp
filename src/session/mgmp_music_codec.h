#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace mgmp::music_codec {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlockMs = 40;
constexpr uint32_t kFrames = kRate * kBlockMs / 1000;
using Pcm = std::array<int16_t, kFrames * 2>;
// Two signed initial samples, two step indices, then one byte per stereo frame.
using Encoded = std::array<uint8_t, 6 + kFrames - 1>;

struct Channel {
    int predictor = 0;
    int index = 0;

    static constexpr int steps[] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,
        60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
        337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
        1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
        5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
        16818,18500,20350,22385,24623,27086,29794,32767,
    };

    int16_t decode(int code) {
        constexpr int changes[] = {-1, -1, -1, -1, 2, 4, 6, 8};
        const int step = steps[index];
        const int delta = (step >> 3) + ((code & 4) ? step : 0) +
                          ((code & 2) ? step >> 1 : 0) + ((code & 1) ? step >> 2 : 0);
        predictor = std::clamp(predictor + ((code & 8) ? -delta : delta), -32768, 32767);
        index = std::clamp(index + changes[code & 7], 0, 88);
        return static_cast<int16_t>(predictor);
    }

    int encode(int sample) {
        int delta = sample - predictor;
        const int sign = delta < 0 ? 8 : 0;
        if (delta < 0) delta = -delta;
        const int code = sign | std::min(7, 4 * delta / steps[index]);
        decode(code);
        return code;
    }
};

inline Encoded encode(const Pcm& pcm, std::array<Channel, 2>& channels) {
    Encoded data{};
    for (unsigned ch = 0; ch < 2; ++ch) {
        channels[ch].predictor = pcm[ch];
        const uint16_t first = static_cast<uint16_t>(pcm[ch]);
        data[ch * 2] = static_cast<uint8_t>(first);
        data[ch * 2 + 1] = static_cast<uint8_t>(first >> 8);
        data[4 + ch] = static_cast<uint8_t>(channels[ch].index);
    }
    for (unsigned i = 1; i < kFrames; ++i)
        data[5 + i] = static_cast<uint8_t>(channels[0].encode(pcm[2 * i]) |
                                         (channels[1].encode(pcm[2 * i + 1]) << 4));
    return data;
}

inline bool decode(const Encoded& data, Pcm& pcm) {
    if (data[4] > 88 || data[5] > 88) return false;
    std::array<Channel, 2> channels{};
    for (unsigned ch = 0; ch < 2; ++ch) {
        const int first = data[2 * ch] | (data[2 * ch + 1] << 8);
        pcm[ch] = static_cast<int16_t>(first >= 32768 ? first - 65536 : first);
        channels[ch] = {pcm[ch], data[4 + ch]};
    }
    for (unsigned i = 1; i < kFrames; ++i) {
        pcm[2 * i] = channels[0].decode(data[5 + i] & 15);
        pcm[2 * i + 1] = channels[1].decode(data[5 + i] >> 4);
    }
    return true;
}

} // namespace mgmp::music_codec
