#include "mgmp_music_codec.h"

#include <cmath>
#include <cstdio>

using namespace mgmp::music_codec;

int main() {
    Pcm source{}, decoded{};
    std::array<Channel, 2> encoder{};
    if (!decode(encode(source, encoder), decoded) || decoded != source) {
        std::puts("silence did not survive a round trip");
        return 1;
    }
    for (unsigned i = 0; i < kFrames; ++i) {
        source[2 * i] = static_cast<int16_t>(9000 * std::sin(i * 0.037));
        source[2 * i + 1] = static_cast<int16_t>(-13000 * std::cos(i * 0.073));
    }
    encode(source, encoder); // Exercise a block following a lost packet.
    auto packet = encode(source, encoder);
    if (!decode(packet, decoded) || decoded[0] != source[0] || decoded[1] != source[1]) {
        std::puts("a stereo packet could not be decoded independently");
        return 1;
    }
    double squared_error = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        const double error = static_cast<int>(source[i]) - decoded[i];
        squared_error += error * error;
    }
    if (std::sqrt(squared_error / source.size()) > 500) {
        std::puts("stereo samples were audibly corrupted by the codec");
        return 1;
    }
    packet[5] = 89;
    if (decode(packet, decoded)) {
        std::puts("an invalid step index was accepted");
        return 1;
    }
    return 0;
}
