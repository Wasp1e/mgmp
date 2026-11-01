// test_tunnel.cpp -- the relay tunnel, end to end, with a fake relay between two
// real tunnels and real loopback sockets on either side.
//
// What this pins: bytes cross intact and in order both ways, a stream bigger than
// the save survives being cut into messages, a full relay backs off and loses
// nothing, and a peer going away is seen as an ordinary socket close -- which is
// the only thing mgmp_net.cpp knows how to react to.
//
// What it cannot pin is Steam itself: that takes two accounts and a real run.
#include "mgmp_tunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// --- the fake relay ----------------------------------------------------------

constexpr uint64_t kHostId = 111, kClientId = 222;

struct Packet { uint64_t from; std::vector<uint8_t> bytes; };

struct Wire {
    CRITICAL_SECTION cs;
    std::deque<Packet> q;
    uint32_t refuse_next = 0;   // how many sends answer Retry first
    Wire()  { InitializeCriticalSection(&cs); }
    ~Wire() { DeleteCriticalSection(&cs); }
};

struct End {
    uint64_t me = 0;
    Wire*    in = nullptr;
    Wire*    out = nullptr;
    volatile LONG closed = 0;
    volatile LONG far_closed = 0;
    End*     other = nullptr;
};

static TunnelSend f_send(void* ctx, uint64_t, const void* data, uint32_t len) {
    End& e = *(End*)ctx;
    EnterCriticalSection(&e.out->cs);
    if (e.out->refuse_next) {
        --e.out->refuse_next;
        LeaveCriticalSection(&e.out->cs);
        return TunnelSend::Retry;
    }
    Packet p;
    p.from = e.me;
    p.bytes.assign((const uint8_t*)data, (const uint8_t*)data + len);
    e.out->q.push_back(std::move(p));
    LeaveCriticalSection(&e.out->cs);
    return TunnelSend::Sent;
}

static int f_recv(void* ctx, TunnelPacket* out, int max) {
    End& e = *(End*)ctx;
    int n = 0;
    EnterCriticalSection(&e.in->cs);
    while (n < max && !e.in->q.empty()) {
        Packet* p = new Packet(std::move(e.in->q.front()));
        e.in->q.pop_front();
        out[n].peer   = p->from;
        out[n].data   = p->bytes.data();
        out[n].len    = (uint32_t)p->bytes.size();
        out[n].handle = p;
        ++n;
    }
    LeaveCriticalSection(&e.in->cs);
    return n;
}

static void f_release(void*, void* handle) { delete (Packet*)handle; }

static TunnelPeer f_state(void* ctx, uint64_t) {
    End& e = *(End*)ctx;
    return InterlockedCompareExchange(&e.far_closed, 0, 0) ? TunnelPeer::Dead : TunnelPeer::Up;
}

static void f_close(void* ctx, uint64_t) {
    End& e = *(End*)ctx;
    InterlockedExchange(&e.closed, 1);
    InterlockedExchange(&e.other->far_closed, 1);
}

static TunnelBackend backend(End& e) {
    TunnelBackend b;
    b.ctx = &e; b.send = f_send; b.recv = f_recv; b.release = f_release;
    b.state = f_state; b.close = f_close;
    return b;
}

// --- plain sockets, standing in for mgmp_net.cpp -----------------------------

static SOCKET listen_any(uint16_t* port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (sockaddr*)&a, sizeof(a));
    listen(l, 4);
    int n = sizeof(a);
    getsockname(l, (sockaddr*)&a, &n);
    *port = ntohs(a.sin_port);
    return l;
}

static SOCKET dial(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    connect(s, (sockaddr*)&a, sizeof(a));
    return s;
}

static void timeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

static bool read_n(SOCKET s, uint8_t* p, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        int r = recv(s, (char*)p + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (uint32_t)r;
    }
    return true;
}

static bool write_n(SOCKET s, const uint8_t* p, uint32_t n) {
    uint32_t off = 0;
    while (off < n) {
        int r = send(s, (const char*)p + off, (int)(n - off), 0);
        if (r <= 0) return false;
        off += (uint32_t)r;
    }
    return true;
}

struct Blob { SOCKET s; const uint8_t* p; uint32_t n; };
static DWORD WINAPI writer(LPVOID param) {
    Blob* b = (Blob*)param;
    write_n(b->s, b->p, b->n);
    return 0;
}

// Each direction is drained on its own thread, because that is what mgmp_net.cpp
// does with every link. The tunnel writes to its loopback sockets blocking, so a
// reader that sat on one side while waiting for the other would stall it.
struct Sink { SOCKET s; uint8_t* p; uint32_t n; volatile LONG ok; };
static DWORD WINAPI reader(LPVOID param) {
    Sink* b = (Sink*)param;
    InterlockedExchange(&b->ok, read_n(b->s, b->p, b->n) ? 1 : 0);
    return 0;
}

static std::vector<uint8_t> pattern(uint32_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) { seed = seed * 2654435761u + 1; v[i] = (uint8_t)(seed >> 24); }
    return v;
}

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Wire to_host, to_client;
    End  host, client;
    host.me   = kHostId;   host.in   = &to_host;   host.out   = &to_client; host.other   = &client;
    client.me = kClientId; client.in = &to_client; client.out = &to_host;   client.other = &host;

    uint16_t game_port = 0;
    SOCKET   game = listen_any(&game_port);

    Tunnel* th = tunnel_start_host(backend(host), game_port);
    uint16_t local = 0;
    Tunnel* tc = tunnel_start_client(backend(client), kHostId, &local);
    check(th && tc && local, "both tunnels start and the client gets a loopback port");

    SOCKET cs = dial(local);
    timeout(cs, 5000);

    printf("-- the handshake shape: the client speaks first --\n");
    const uint8_t hello[] = "hello";
    check(write_n(cs, hello, sizeof(hello)), "client writes");
    SOCKET hs = accept(game, nullptr, nullptr);
    check(hs != INVALID_SOCKET, "the host's listener sees a connection on first bytes");
    timeout(hs, 5000);
    uint8_t got[sizeof(hello)] = {};
    check(read_n(hs, got, sizeof(got)) && !memcmp(got, hello, sizeof(got)), "host reads them");

    const uint8_t reply[] = "welcome";
    check(write_n(hs, reply, sizeof(reply)), "host answers");
    uint8_t got2[sizeof(reply)] = {};
    check(read_n(cs, got2, sizeof(got2)) && !memcmp(got2, reply, sizeof(got2)), "client reads it");

    printf("-- a stream far bigger than one message, both ways at once --\n");
    {
        const uint32_t n = 3 * 1024 * 1024;
        auto a = pattern(n, 1), b = pattern(n, 2);
        Blob ba{ cs, a.data(), n }, bb{ hs, b.data(), n };
        HANDLE w1 = CreateThread(nullptr, 0, writer, &ba, 0, nullptr);
        HANDLE w2 = CreateThread(nullptr, 0, writer, &bb, 0, nullptr);
        std::vector<uint8_t> ra(n), rb(n);
        Sink sa{ hs, ra.data(), n, 0 }, sb{ cs, rb.data(), n, 0 };
        HANDLE r1 = CreateThread(nullptr, 0, reader, &sa, 0, nullptr);
        HANDLE r2 = CreateThread(nullptr, 0, reader, &sb, 0, nullptr);
        WaitForSingleObject(r1, 20000); WaitForSingleObject(r2, 20000);
        check(sa.ok && ra == a, "client -> host intact and in order");
        check(sb.ok && rb == b, "host -> client intact and in order");
        WaitForSingleObject(w1, 5000); WaitForSingleObject(w2, 5000);
        CloseHandle(w1); CloseHandle(w2); CloseHandle(r1); CloseHandle(r2);
    }

    printf("-- a full relay backs off and loses nothing --\n");
    {
        EnterCriticalSection(&to_host.cs);
        to_host.refuse_next = 200;
        LeaveCriticalSection(&to_host.cs);

        const uint32_t n = 200 * 1024;
        auto a = pattern(n, 3);
        Blob ba{ cs, a.data(), n };
        HANDLE w = CreateThread(nullptr, 0, writer, &ba, 0, nullptr);
        std::vector<uint8_t> ra(n);
        check(read_n(hs, ra.data(), n) && ra == a, "every byte arrives after the retries");
        WaitForSingleObject(w, 5000);
        CloseHandle(w);
    }

    printf("-- a session ending is an ordinary socket close on the far side --\n");
    {
        closesocket(cs);
        uint8_t b = 0;
        int r = recv(hs, (char*)&b, 1, 0);
        check(r == 0, "the host's socket sees an orderly shutdown");
        check(InterlockedCompareExchange(&client.closed, 0, 0) != 0, "the client told the relay");
    }

    printf("-- the relay dropping a peer closes the local socket --\n");
    {
        // A fresh client arrives, then the relay reports it gone.
        InterlockedExchange(&host.far_closed, 0);
        InterlockedExchange(&host.closed, 0);
        closesocket(hs);
        Sleep(50);

        Packet p;
        p.from = kClientId + 1;
        p.bytes.assign(3, 7);
        EnterCriticalSection(&to_host.cs);
        to_host.q.push_back(p);
        LeaveCriticalSection(&to_host.cs);

        SOCKET h2 = accept(game, nullptr, nullptr);
        check(h2 != INVALID_SOCKET, "a second peer gets its own connection");
        timeout(h2, 5000);
        uint8_t b[3] = {};
        check(read_n(h2, b, 3) && b[0] == 7, "and its bytes");

        InterlockedExchange(&host.far_closed, 1);
        uint8_t x = 0;
        check(recv(h2, (char*)&x, 1, 0) == 0, "the relay's Dead closes the socket");
        closesocket(h2);
    }

    tunnel_stop(tc);
    tunnel_stop(th);
    tunnel_stop(nullptr);
    check(true, "both tunnels stop, and stopping nullptr is safe");

    closesocket(game);
    WSACleanup();

    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
