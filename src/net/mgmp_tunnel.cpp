// mgmp_tunnel.cpp -- see mgmp_tunnel.h.

#include "mgmp_tunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace mgmp {

namespace {

// Four players is the protocol's ceiling, so three remote peers at most.
constexpr int kMaxSlots = 4;

// Well under Steam's 512 KB message limit. A save is ~45 KB and goes as a few of
// these; nothing else comes close to filling one.
constexpr uint32_t kChunk = 32 * 1024;

constexpr int   kBatch       = 32;
constexpr DWORD kStateEveryMs = 500;

struct Slot {
    uint64_t peer     = 0;
    SOCKET   sock     = INVALID_SOCKET;
    bool     ever_up  = false;
    uint32_t pending  = 0;             // bytes in buf the backend refused so far
    uint8_t  buf[kChunk];
};

} // namespace

struct Tunnel {
    TunnelBackend b;
    bool     host     = false;
    uint16_t port     = 0;             // host: where net_host is listening
    uint64_t remote   = 0;             // client: the one peer
    SOCKET   listener = INVALID_SOCKET;
    HANDLE   thread   = nullptr;
    volatile LONG stop = 0;
    bool     wsa_up   = false;
    Slot     slots[kMaxSlots];
};

namespace {

void tlog(Tunnel& t, const char* fmt, ...) {
    if (!t.b.log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    t.b.log(t.b.ctx, line);
}

Slot* slot_of(Tunnel& t, uint64_t peer) {
    for (Slot& s : t.slots)
        if (s.sock != INVALID_SOCKET && s.peer == peer) return &s;
    return nullptr;
}

Slot* free_slot(Tunnel& t) {
    for (Slot& s : t.slots)
        if (s.sock == INVALID_SOCKET) return &s;
    return nullptr;
}

// Closing the socket is how mgmp_net.cpp hears that a peer went: its receive
// thread sees an orderly shutdown and runs the path it has always run.
void drop(Tunnel& t, Slot& s, const char* why) {
    tlog(t, "peer %llu closed: %s", (unsigned long long)s.peer, why);
    closesocket(s.sock);
    s.sock    = INVALID_SOCKET;
    s.pending = 0;
    s.ever_up = false;
    t.b.close(t.b.ctx, s.peer);
}

void nodelay_on(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

SOCKET dial_loopback(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);
    if (connect(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nodelay_on(s);
    return s;
}

bool write_all(SOCKET s, const uint8_t* p, uint32_t n) {
    uint32_t off = 0;
    while (off < n) {
        int r = send(s, (const char*)p + off, (int)(n - off), 0);
        if (r <= 0) return false;
        off += (uint32_t)r;
    }
    return true;
}

void deliver(Tunnel& t, const TunnelPacket& p) {
    Slot* s = slot_of(t, p.peer);

    if (!s && t.host) {
        s = free_slot(t);
        if (!s) {
            tlog(t, "refused peer %llu -- every slot is in use", (unsigned long long)p.peer);
            t.b.close(t.b.ctx, p.peer);
            return;
        }
        SOCKET c = dial_loopback(t.port);
        if (c == INVALID_SOCKET) {
            tlog(t, "could not reach the local host socket on port %u", (unsigned)t.port);
            t.b.close(t.b.ctx, p.peer);
            return;
        }
        s->sock    = c;
        s->peer    = p.peer;
        s->pending = 0;
        s->ever_up = false;
        tlog(t, "peer %llu arrived", (unsigned long long)p.peer);
    }

    // A straggler from a peer that is already gone, or on a client someone other
    // than the host. Neither has anywhere to go.
    if (!s) return;

    if (!write_all(s->sock, (const uint8_t*)p.data, p.len)) drop(t, *s, "local socket write failed");
}

void pump_backend(Tunnel& t) {
    // Not before net_join has connected: anything read now would have nowhere to
    // go. The backend holds it until there is.
    if (!t.host && !slot_of(t, t.remote)) return;

    TunnelPacket p[kBatch];
    for (;;) {
        int n = t.b.recv(t.b.ctx, p, kBatch);
        if (n <= 0) return;
        for (int i = 0; i < n; ++i) {
            deliver(t, p[i]);
            t.b.release(t.b.ctx, p[i].handle);
        }
        if (n < kBatch) return;
    }
}

void flush(Tunnel& t, Slot& s) {
    switch (t.b.send(t.b.ctx, s.peer, s.buf, s.pending)) {
        case TunnelSend::Sent:   s.pending = 0; break;
        case TunnelSend::Retry:  break;
        case TunnelSend::Failed: drop(t, s, "the relay refused the send"); break;
    }
}

void accept_client(Tunnel& t) {
    SOCKET c = accept(t.listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) return;
    Slot* s = free_slot(t);
    if (!s || slot_of(t, t.remote)) { closesocket(c); return; }
    nodelay_on(c);
    s->sock    = c;
    s->peer    = t.remote;
    s->pending = 0;
    s->ever_up = false;
}

void pump_sockets(Tunnel& t) {
    fd_set rd;
    FD_ZERO(&rd);
    int armed = 0;

    if (t.listener != INVALID_SOCKET) { FD_SET(t.listener, &rd); ++armed; }
    for (Slot& s : t.slots) {
        if (s.sock == INVALID_SOCKET) continue;
        if (s.pending) { flush(t, s); if (s.pending || s.sock == INVALID_SOCKET) continue; }
        FD_SET(s.sock, &rd);
        ++armed;
    }

    // select() rejects three empty sets on Windows instead of sleeping.
    if (!armed) { Sleep(1); return; }

    timeval wait{ 0, 1000 };
    if (select(0, &rd, nullptr, nullptr, &wait) <= 0) return;

    if (t.listener != INVALID_SOCKET && FD_ISSET(t.listener, &rd)) accept_client(t);

    for (Slot& s : t.slots) {
        if (s.sock == INVALID_SOCKET || s.pending || !FD_ISSET(s.sock, &rd)) continue;
        int r = recv(s.sock, (char*)s.buf, (int)kChunk, 0);
        if (r <= 0) { drop(t, s, "the local session ended"); continue; }
        s.pending = (uint32_t)r;
        flush(t, s);
    }
}

void poll_states(Tunnel& t) {
    for (Slot& s : t.slots) {
        if (s.sock == INVALID_SOCKET) continue;
        TunnelPeer st = t.b.state(t.b.ctx, s.peer);
        if (st == TunnelPeer::Up) s.ever_up = true;
        // None before Up is a session that has not started, on a client whose
        // first bytes have not gone out yet. None after Up is one that ended.
        if (st == TunnelPeer::Dead || (st == TunnelPeer::None && s.ever_up))
            drop(t, s, "the relay lost the peer");
    }
}

DWORD WINAPI tunnel_thread(LPVOID param) {
    Tunnel& t = *(Tunnel*)param;
    DWORD last = GetTickCount();

    while (!InterlockedCompareExchange(&t.stop, 0, 0)) {
        pump_backend(t);
        pump_sockets(t);

        DWORD now = GetTickCount();
        if (now - last >= kStateEveryMs) {
            last = now;
            poll_states(t);
        }
    }

    for (Slot& s : t.slots)
        if (s.sock != INVALID_SOCKET) drop(t, s, "the tunnel stopped");
    return 0;
}

Tunnel* make(const TunnelBackend& b) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
    Tunnel* t = new Tunnel();
    t->b      = b;
    t->wsa_up = true;
    return t;
}

bool run(Tunnel* t) {
    t->thread = CreateThread(nullptr, 0, tunnel_thread, t, 0, nullptr);
    return t->thread != nullptr;
}

void destroy(Tunnel* t) {
    if (t->listener != INVALID_SOCKET) closesocket(t->listener);
    if (t->wsa_up) WSACleanup();
    delete t;
}

} // namespace

Tunnel* tunnel_start_host(const TunnelBackend& b, uint16_t port) {
    Tunnel* t = make(b);
    if (!t) return nullptr;
    t->host = true;
    t->port = port;
    if (!run(t)) { destroy(t); return nullptr; }
    return t;
}

Tunnel* tunnel_start_client(const TunnelBackend& b, uint64_t remote, uint16_t* out_port) {
    Tunnel* t = make(b);
    if (!t) return nullptr;
    t->remote = remote;

    t->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    int alen = sizeof(a);
    if (t->listener == INVALID_SOCKET ||
        bind(t->listener, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR ||
        listen(t->listener, 1) == SOCKET_ERROR ||
        getsockname(t->listener, (sockaddr*)&a, &alen) == SOCKET_ERROR) {
        destroy(t);
        return nullptr;
    }
    if (out_port) *out_port = ntohs(a.sin_port);

    if (!run(t)) { destroy(t); return nullptr; }
    return t;
}

void tunnel_stop(Tunnel* t) {
    if (!t) return;
    InterlockedExchange(&t->stop, 1);
    if (t->thread) {
        WaitForSingleObject(t->thread, 2000);
        CloseHandle(t->thread);
    }
    destroy(t);
}

} // namespace mgmp
