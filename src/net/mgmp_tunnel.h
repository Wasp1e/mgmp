// mgmp_tunnel.h -- carry the TCP transport over something that is not TCP.
//
// mgmp_net.cpp is a byte stream with a lot of play behind it, and its framing,
// relay, reconnect and shutdown paths have all been measured. So a relay does not
// replace it. It sits underneath: one loopback socket per remote peer, and a
// thread that moves bytes between that socket and a message transport.
//
//   client:  net_join(127.0.0.1:<ours>) <-> tunnel <-> relay <-> tunnel <-> net_host
//
// The backend is a table of functions rather than a direct dependency on Steam
// so tests/test_tunnel.cpp can run all of this with two tunnels and no Steam.
//
// The backend must deliver reliably and in order, per peer. Message boundaries do
// not matter: this is a stream, and mgmp_net.cpp finds its own frames in it.
#pragma once

#include <cstdint>

namespace mgmp {

enum class TunnelPeer { None, Connecting, Up, Dead };

enum class TunnelSend { Sent, Retry, Failed };

struct TunnelPacket {
    uint64_t    peer   = 0;
    const void* data   = nullptr;
    uint32_t    len    = 0;
    void*       handle = nullptr;      // passed back to release()
};

struct TunnelBackend {
    void* ctx = nullptr;

    // Retry means the backend's own send buffer is full. The same bytes are
    // offered again, so nothing may be consumed on that path.
    TunnelSend (*send)(void* ctx, uint64_t peer, const void* data, uint32_t len) = nullptr;
    int        (*recv)(void* ctx, TunnelPacket* out, int max) = nullptr;
    void       (*release)(void* ctx, void* handle) = nullptr;
    TunnelPeer (*state)(void* ctx, uint64_t peer) = nullptr;
    void       (*close)(void* ctx, uint64_t peer) = nullptr;

    // Optional. The tunnel has no opinion on where its log goes.
    void       (*log)(void* ctx, const char* line) = nullptr;
};

struct Tunnel;

// Host side: each remote peer gets a loopback connection to `port`, opened when
// its first bytes arrive. Client side, see tunnel_start_client.
Tunnel* tunnel_start_host(const TunnelBackend& b, uint16_t port);

// Listens on an ephemeral loopback port, written to *out_port, for the one
// connection net_join is about to make, and carries it to `remote`.
Tunnel* tunnel_start_client(const TunnelBackend& b, uint64_t remote, uint16_t* out_port);

// Joins the thread, closes every socket and frees the tunnel. Safe on nullptr.
void tunnel_stop(Tunnel* t);

} // namespace mgmp
