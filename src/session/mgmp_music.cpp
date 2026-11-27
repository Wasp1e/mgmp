#include "mgmp_music.h"
#include "mgmp_music_codec.h"
#include "mgmp_net.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "MinHook.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mgmp {
namespace {

using namespace music_codec;
using Clock = std::chrono::steady_clock;

constexpr uint64_t kMixDelayUs = 240000;
constexpr uint64_t kBlockUs = kBlockMs * 1000;
constexpr uint64_t kProbeEveryUs = 100000;
constexpr uint64_t kPeerTimeoutUs = 1000000;
constexpr uint64_t kDriftUs = 30000;
constexpr size_t kHistory = 24;
constexpr unsigned kMaxUnacknowledged = 8;
constexpr unsigned kMaxBurst = 3;
constexpr float kPcmScale = 8192.0f; // Leave headroom for the mix before MusicVolume.

enum MusicFrame : uint8_t { Audio = 1, ClockProbe, ClockReply };

struct Block {
    uint64_t number = 0;
    uint64_t captured = 0;
    Pcm pcm{};
};

struct Peer {
    uint64_t ping = 0;
    uint64_t ping_received = 0;
    uint64_t expires = 0;
    uint64_t acknowledged = 0;
    uint64_t last_ahead = 0;
    unsigned ahead = 0;
};

// The mixer never waits for a network read, game frame, log call or encoder.
std::mutex mutex;
NetRole role = NetRole::None;
std::array<Block, kHistory> history;
std::array<Peer, kMaxPeers> peers;
Block capture;
unsigned captured_frames = 0;
uint64_t newest = 0;
uint64_t sent = 0;
uint64_t last_mix = 0;
uint64_t play_frame = 0;
uint64_t probe = 0;
uint64_t best_rtt = 0;
uint64_t clock_sample = 0;
int64_t host_offset = 0;
bool clock_ready = false;
bool playing_reported = false;
std::array<Channel, 2> encoder{};
std::atomic<bool> hooked{false};
std::atomic<bool> client_music{false};
std::atomic<uint64_t> played{0};

using Mix = void (*)(void*);
Mix original_mix = nullptr;
void** audio_system = nullptr;

uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count();
}

void remember(const Block& block) {
    history[block.number % kHistory] = block;
    newest = block.number;
}

bool host_has_listeners(uint64_t now) {
    for (uint8_t peer = 1; peer < kMaxPeers; ++peer)
        if (peers[peer].expires > now) return true;
    return false;
}

void capture_mix(const float* samples, unsigned frames, uint64_t now) {
    if (now - last_mix > kMixDelayUs / 2) {
        captured_frames = 0;
        capture.number = std::max(newest + 1, now / kBlockUs);
    }
    last_mix = now;
    for (unsigned done = 0; done < frames;) {
        if (!captured_frames) capture.captured = now + done * 1000000ull / kRate;
        const unsigned count = std::min(frames - done, kFrames - captured_frames);
        for (unsigned i = 0; i < count * 2; ++i) {
            const float value = samples[done * 2 + i];
            capture.pcm[captured_frames * 2 + i] = std::isfinite(value)
                ? static_cast<int16_t>(std::clamp(value * kPcmScale, -32768.0f, 32767.0f)) : 0;
        }
        captured_frames += count;
        done += count;
        if (captured_frames != kFrames) continue;
        remember(capture);
        ++capture.number;
        captured_frames = 0;
    }
}

void replace_mix(float* samples, unsigned frames, uint64_t now) {
    // The client's native player keeps advancing even while receiving host
    // audio. Every early return and missing block must therefore start silent.
    if (role == NetRole::Client) std::fill_n(samples, frames * 2, 0.0f);
    if (!newest || (role == NetRole::Client && !clock_ready)) return;
    const Block& latest = history[newest % kHistory];
    const int64_t elapsed = static_cast<int64_t>(now) + host_offset -
                            static_cast<int64_t>(latest.captured);
    if (elapsed > static_cast<int64_t>(kPeerTimeoutUs) || elapsed < -static_cast<int64_t>(kBlockUs)) {
        play_frame = 0;
        return;
    }
    const int64_t wanted = static_cast<int64_t>(newest * kFrames) +
                           (elapsed - static_cast<int64_t>(kMixDelayUs)) * kRate / 1000000;
    if (wanted <= 0) return;
    if (!play_frame || std::abs(wanted - static_cast<int64_t>(play_frame)) >
                       static_cast<int64_t>(kDriftUs * kRate / 1000000))
        play_frame = static_cast<uint64_t>(wanted);

    bool replaced = false;
    for (unsigned done = 0; done < frames;) {
        const uint64_t number = play_frame / kFrames;
        const unsigned offset = static_cast<unsigned>(play_frame % kFrames);
        const unsigned count = std::min(frames - done, kFrames - offset);
        const Block& block = history[number % kHistory];
        if (number != 0 && block.number == number) {
            for (unsigned i = 0; i < count * 2; ++i)
                samples[done * 2 + i] = block.pcm[offset * 2 + i] / kPcmScale;
            replaced = true;
        } else if (role == NetRole::Client && played.load(std::memory_order_relaxed)) {
            std::fill_n(samples + done * 2, count * 2, 0.0f);
        }
        done += count;
        play_frame += count;
    }
    if (replaced) played.fetch_add(frames, std::memory_order_relaxed);
}

// FAudio's internal submix callback receives interleaved floats before the
// submix volume, filters and effects. The game creates music_main at 48 kHz in
// stereo and stores its wrapper in AudioSystem+0x18. The input allocation has
// two FAudio resampler padding frames, which we leave alone.
void mix_music(void* voice) {
    const auto* system = static_cast<const uint8_t*>(*audio_system);
    if (system) {
        const auto* bus = *reinterpret_cast<void* const*>(system + 0x18);
        auto* v = static_cast<uint8_t*>(voice);
        if (bus && voice == *static_cast<void* const*>(bus) &&
            *reinterpret_cast<const uint32_t*>(v + 0xE8) == 2 &&
            *reinterpret_cast<const uint32_t*>(v + 0xEC) == kRate) {
            const unsigned count = *reinterpret_cast<const uint32_t*>(v + 0xC8) / 2;
            auto* samples = *reinterpret_cast<float**>(v + 0xD0);
            if (samples && count > 2 && count <= 8192) {
                // A busy network/game thread must not expose the local track.
                // This flag is independent of the state mutex so the audio
                // thread can silence the bus even when try_lock fails.
                if (client_music.load(std::memory_order_acquire))
                    std::fill_n(samples, count * 2, 0.0f);
                std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
                if (lock.owns_lock() && role != NetRole::None) {
                    const uint64_t now = now_us();
                    if (role == NetRole::Client || host_has_listeners(now)) {
                        if (role == NetRole::Host) capture_mix(samples, count - 2, now);
                        replace_mix(samples, count - 2, now);
                    }
                }
            }
        }
    }
    original_mix(voice);
}

} // namespace

void music_set_base(uintptr_t base) {
    constexpr uintptr_t kMix = 0xD88F20;
    constexpr uintptr_t kMusicBusCall = 0x954F74;
    constexpr uintptr_t kAudioSystem = 0x13C1940;
    constexpr uint8_t mix_head[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x40};
    constexpr uint8_t bus_anchor[] = {0xE8, 0x67, 0x6C, 0x0E, 0x00, 0x48, 0x89, 0x43, 0x18};
    uint8_t head[sizeof(mix_head)]{}, anchor[sizeof(bus_anchor)]{};
    auto* at = reinterpret_cast<void*>(base + kMix);
    if (!mem_read(at, head, sizeof(head)) || memcmp(head, mix_head, sizeof(head)) ||
        !mem_read(reinterpret_cast<void*>(base + kMusicBusCall), anchor, sizeof(anchor)) ||
        memcmp(anchor, bus_anchor, sizeof(anchor))) {
        log_line("MUSIC", "music mixer signature did not match; local playback");
        return;
    }
    audio_system = reinterpret_cast<void**>(base + kAudioSystem);
    if (MH_CreateHook(at, reinterpret_cast<void*>(&mix_music), reinterpret_cast<void**>(&original_mix)) != MH_OK ||
        MH_EnableHook(at) != MH_OK) {
        log_line("MUSIC", "could not hook music mixer; local playback");
        return;
    }
    hooked = true;
    log_line("MUSIC", "48 kHz stereo music mixer hooked");
}

void music_reset() {
    std::lock_guard<std::mutex> lock(mutex);
    client_music.store(false, std::memory_order_release);
    role = NetRole::None;
    for (Block& block : history) block.number = 0;
    peers = {};
    captured_frames = 0;
    newest = sent = last_mix = play_frame = probe = best_rtt = clock_sample = 0;
    host_offset = 0;
    clock_ready = playing_reported = false;
    encoder = {};
    played = 0;
}

void music_receive(const uint8_t* data, uint32_t size, uint8_t from, bool host) {
    if (!hooked || size < 2 || from >= kMaxPeers || (host ? from == kHostPeer : from != kHostPeer))
        return;
    Reader r(data, size);
    r.u8v(); // MSG_MUSIC
    const uint8_t kind = r.u8v();
    const uint64_t now = now_us();
    if (kind == Audio && !host) {
        Block block;
        block.number = r.u64v();
        block.captured = r.u64v();
        Encoded bytes;
        r.raw(bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (!r.ok || r.pos != size || !block.captured || block.captured > INT64_MAX / 4 ||
            !block.number || block.number > INT64_MAX / 2 / kFrames || !decode(bytes, block.pcm))
            return;
        std::lock_guard<std::mutex> lock(mutex);
        if (role == NetRole::Client && block.number > newest) remember(block);
        return;
    }
    const uint64_t ping = r.u64v();
    const uint64_t value = r.u64v();
    const uint64_t reply_time = kind == ClockReply ? r.u64v() : 0;
    if (!r.ok || r.pos != size || !ping || ping > INT64_MAX / 4 ||
        value > INT64_MAX / 4 || reply_time > INT64_MAX / 4)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    if (kind == ClockProbe && host && role == NetRole::Host) {
        Peer& peer = peers[from];
        if (now >= peer.expires) peer = {};
        peer.ping = ping;
        peer.ping_received = now;
        peer.expires = now + kPeerTimeoutUs;
        if (value > peer.acknowledged && value <= sent) {
            peer.acknowledged = value;
            peer.ahead = 0;
        }
    } else if (kind == ClockReply && !host && role == NetRole::Client && ping <= probe &&
               ping <= now && now - ping <= kPeerTimeoutUs && ping > clock_sample &&
               reply_time >= value && reply_time - value <= now - ping) {
        const uint64_t rtt = now - ping - (reply_time - value);
        if (!clock_ready || rtt <= best_rtt + 3000 || ping - clock_sample > kPeerTimeoutUs * 4) {
            const int64_t estimate = static_cast<int64_t>(value) - static_cast<int64_t>(ping + rtt / 2);
            host_offset = clock_ready ? host_offset + (estimate - host_offset) / 4 : estimate;
            best_rtt = rtt;
            clock_sample = ping;
            clock_ready = true;
        }
    }
}

void music_pump() {
    if (!hooked) return;
    const NetRole current = net_active() ? net_role() : NetRole::None;
    if (current != role) {
        music_reset();
        std::lock_guard<std::mutex> lock(mutex);
        role = current;
        client_music.store(current == NetRole::Client, std::memory_order_release);
    }
    if (current == NetRole::None) return;
    const uint64_t now = now_us();
    if (current == NetRole::Client) {
        if (!playing_reported && played.load(std::memory_order_relaxed)) {
            playing_reported = true;
            log_line("MUSIC", "playing the host's stereo music mix");
        }
        if (now - probe < kProbeEveryUs) return;
        uint64_t acknowledged;
        {
            std::lock_guard<std::mutex> lock(mutex);
            probe = now;
            acknowledged = newest;
        }
        uint8_t frame[18];
        Writer w(frame, sizeof(frame));
        w.u8v(MSG_MUSIC); w.u8v(ClockProbe); w.u64v(now); w.u64v(acknowledged);
        net_send_music_to(kHostPeer, frame, w.len);
        return;
    }

    std::array<Block, kMaxBurst> blocks;
    unsigned n = 0;
    std::array<Peer, kMaxPeers> targets{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (sent < newest) {
            const uint64_t first = newest - std::min<uint64_t>(newest - sent, kMaxBurst) + 1;
            for (uint64_t number = first; number <= newest; ++number) {
                const Block& block = history[number % kHistory];
                if (block.number == number && now < block.captured + kMixDelayUs)
                    blocks[n++] = block;
            }
            sent = newest;
        }
        for (uint8_t peer = 1; peer < kMaxPeers; ++peer) {
            Peer& state = peers[peer];
            if (state.expires <= now) continue;
            targets[peer] = state;
            targets[peer].ahead = 0;
            state.ping = 0;
            // Permit one retry after a full window even if the preceding
            // packets could not be sent. Credits stay bounded on a slow link.
            if (state.ahead >= kMaxUnacknowledged && now - state.last_ahead >= kPeerTimeoutUs)
                state.ahead = kMaxUnacknowledged - 1;
            const unsigned count = std::min(n, kMaxUnacknowledged - state.ahead);
            targets[peer].ahead = count;
            state.ahead += count;
            if (count && state.ahead == kMaxUnacknowledged) state.last_ahead = now;
        }
    }

    uint8_t frame[18 + Encoded{}.size()];
    for (unsigned i = 0; i < n; ++i) {
        const Block& block = blocks[i];
        const Encoded bytes = encode(block.pcm, encoder);
        Writer w(frame, sizeof(frame));
        w.u8v(MSG_MUSIC); w.u8v(Audio); w.u64v(block.number); w.u64v(block.captured);
        w.raw(bytes.data(), static_cast<uint32_t>(bytes.size()));
        for (uint8_t peer = 1; peer < kMaxPeers; ++peer)
            if (targets[peer].ahead >= n - i) net_send_music_to(peer, frame, w.len);
    }
    for (uint8_t peer = 1; peer < kMaxPeers; ++peer) {
        if (!targets[peer].ping) continue;
        Writer w(frame, sizeof(frame));
        w.u8v(MSG_MUSIC); w.u8v(ClockReply); w.u64v(targets[peer].ping);
        w.u64v(targets[peer].ping_received); w.u64v(now_us());
        net_send_music_to(peer, frame, w.len);
    }
}

} // namespace mgmp
