// mgmp_steamlink.cpp -- see mgmp_steamlink.h.
//
// The layouts and constants below are copied from Valve's public headers
// (steamnetworkingtypes.h, isteamnetworkingmessages.h, steam_api_internal.h), not
// inferred. The flat API takes `this` as its first argument.

#include "mgmp_steamlink.h"
#include "mgmp_log.h"
#include "mgmp_tunnel.h"

#include <windows.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// SteamNetworkingIdentity: pack(1), {int type; int size; 128-byte union}. Only ever
// filled and read by the two flat helpers, so its contents stay Valve's business.
struct Identity { uint8_t raw[136]; };
static_assert(sizeof(Identity) == 136, "SteamNetworkingIdentity is 136 bytes");

// The prefix of SteamNetworkingMessage_t this file reads.
struct Message {
    void*    data;
    int32_t  size;
    uint32_t conn;
    Identity peer;
};
static_assert(offsetof(Message, peer) == 16, "SteamNetworkingMessage_t layout");

// Must match the class steam_api64.dll was built against, and only an MSVC build
// of the same declaration lays the overloads out the same way -- which this is.
class CallbackBase {
public:
    CallbackBase() { flags = 0; id = 0; }
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, uint64_t call) = 0;
    virtual int  GetCallbackSizeBytes() = 0;
protected:
    uint8_t flags;
    int     id;
};

constexpr int kCb_MessagesSessionRequest = 1250 + 1;   // k_iSteamNetworkingMessagesCallbacks + 1

constexpr int kSend_ReliableNoNagle = 8 | 1;
constexpr int kSend_AutoRestart     = 32;

constexpr int kResult_OK            = 1;
constexpr int kResult_LimitExceeded = 25;

constexpr int kState_Connecting     = 1;
constexpr int kState_FindingRoute   = 2;
constexpr int kState_Connected      = 3;
constexpr int kState_ClosedByPeer   = 4;
constexpr int kState_ProblemLocally = 5;

constexpr int kFriendImmediate = 4;   // k_EFriendFlagImmediate

// One channel for the whole mod, chosen to stay clear of anything the game might
// ever open: the same number as the default TCP port.
constexpr int kChannel = 27600;

struct Api {
    void* messages = nullptr;
    void* friends  = nullptr;
    void* user     = nullptr;

    int  (__cdecl* send)(void*, const Identity*, const void*, uint32_t, int, int) = nullptr;
    int  (__cdecl* recv)(void*, int, Message**, int) = nullptr;
    bool (__cdecl* accept)(void*, const Identity*) = nullptr;
    bool (__cdecl* close)(void*, const Identity*) = nullptr;
    int  (__cdecl* info)(void*, const Identity*, void*, void*) = nullptr;
    void (__cdecl* release)(Message*) = nullptr;
    void (__cdecl* set_id)(Identity*, uint64_t) = nullptr;
    uint64_t (__cdecl* get_id)(Identity*) = nullptr;
    bool (__cdecl* has_friend)(void*, uint64_t, int) = nullptr;
    uint64_t (__cdecl* self_id)(void*) = nullptr;
    void (__cdecl* reg)(CallbackBase*, int) = nullptr;
    void (__cdecl* unreg)(CallbackBase*) = nullptr;
    bool ok = false;
};

struct State {
    Api     api;
    Tunnel* tunnel = nullptr;
    char    error[192] = {};
} g;

void set_error(const char* what) {
    _snprintf_s(g.error, sizeof(g.error), _TRUNCATE, "%s", what);
    log_line("STEAM", "!! %s", what);
}

template <class T>
bool bind(HMODULE m, T& out, const char* name) {
    out = (T)GetProcAddress(m, name);
    if (!out) log_line("STEAM", "!! steam_api64.dll has no %s", name);
    return out != nullptr;
}

Identity identity_of(uint64_t id) {
    Identity i{};
    g.api.set_id(&i, id);
    return i;
}

bool resolve() {
    if (g.api.ok) return true;

    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) { set_error("steam_api64.dll is not loaded -- start the game through Steam"); return false; }

    void* (__cdecl* get_messages)() = nullptr;
    void* (__cdecl* get_utils)()    = nullptr;
    void* (__cdecl* get_user)()     = nullptr;
    void* (__cdecl* get_friends)()  = nullptr;
    int   (__cdecl* init_relay)(void*) = nullptr;

    Api a;
    bool ok = true;
    ok &= bind(m, get_messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    ok &= bind(m, get_utils,    "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    ok &= bind(m, get_user,     "SteamAPI_SteamUser_v023");
    ok &= bind(m, get_friends,  "SteamAPI_SteamFriends_v018");
    ok &= bind(m, init_relay,   "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    ok &= bind(m, a.send,       "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    ok &= bind(m, a.recv,       "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    ok &= bind(m, a.accept,     "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    ok &= bind(m, a.close,      "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    ok &= bind(m, a.info,       "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
    ok &= bind(m, a.release,    "SteamAPI_SteamNetworkingMessage_t_Release");
    ok &= bind(m, a.set_id,     "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    ok &= bind(m, a.get_id,     "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    ok &= bind(m, a.has_friend, "SteamAPI_ISteamFriends_HasFriend");
    ok &= bind(m, a.self_id,    "SteamAPI_ISteamUser_GetSteamID");
    ok &= bind(m, a.reg,        "SteamAPI_RegisterCallback");
    ok &= bind(m, a.unreg,      "SteamAPI_UnregisterCallback");
    if (!ok) { set_error("this Steam build does not export what the relay needs"); return false; }

    a.messages = get_messages();
    a.friends  = get_friends();
    a.user     = get_user();
    void* utils = get_utils();
    if (!a.messages || !a.friends || !a.user || !utils) {
        set_error("Steam is not up yet -- is the Steam client running and logged in?");
        return false;
    }

    // Optional, and it only shortens the first connection: Steam would otherwise
    // fetch the relay network on demand.
    init_relay(utils);

    a.ok  = true;
    g.api = a;
    log_line("STEAM", "relay API resolved, this account is %llu",
             (unsigned long long)g.api.self_id(g.api.user));
    return true;
}

// --- the host's front door ---------------------------------------------------
//
// Runs on the game thread, from inside its own SteamAPI_RunCallbacks.

class SessionRequest : public CallbackBase {
public:
    void Run(void* param) override {
        Identity who;
        memcpy(&who, param, sizeof(who));
        const uint64_t id = g.api.get_id(&who);

        // A SteamID is not a secret, and what comes through here reaches the save
        // loader. Nobody who is not a friend of the host gets as far as a socket.
        if (!g.api.has_friend(g.api.friends, id, kFriendImmediate)) {
            log_line("STEAM", "!! refused %llu -- not on this account's friends list",
                     (unsigned long long)id);
            return;
        }
        log_line("STEAM", "accepting friend %llu", (unsigned long long)id);
        g.api.accept(g.api.messages, &who);
    }
    void Run(void* param, bool, uint64_t) override { Run(param); }
    int  GetCallbackSizeBytes() override { return (int)sizeof(Identity); }

    void arm(int cb) { flags = 0; id = cb; }
};

SessionRequest g_request;
bool           g_request_armed = false;

void arm_requests() {
    if (g_request_armed) return;
    g_request.arm(kCb_MessagesSessionRequest);
    g.api.reg(&g_request, kCb_MessagesSessionRequest);
    g_request_armed = true;
}

void disarm_requests() {
    if (!g_request_armed) return;
    g.api.unreg(&g_request);
    g_request_armed = false;
}

// --- the backend -------------------------------------------------------------

TunnelSend b_send(void*, uint64_t peer, const void* data, uint32_t len) {
    Identity who = identity_of(peer);
    int r = g.api.send(g.api.messages, &who, data, len,
                       kSend_ReliableNoNagle | kSend_AutoRestart, kChannel);
    if (r == kResult_OK)            return TunnelSend::Sent;
    if (r == kResult_LimitExceeded) return TunnelSend::Retry;
    log_line("STEAM", "!! SendMessageToUser -> EResult %d", r);
    return TunnelSend::Failed;
}

int b_recv(void*, TunnelPacket* out, int max) {
    Message* msgs[32];
    if (max > 32) max = 32;
    int n = g.api.recv(g.api.messages, kChannel, msgs, max);
    for (int i = 0; i < n; ++i) {
        out[i].peer   = g.api.get_id(&msgs[i]->peer);
        out[i].data   = msgs[i]->data;
        out[i].len    = (uint32_t)msgs[i]->size;
        out[i].handle = msgs[i];
    }
    return n;
}

void b_release(void*, void* handle) { g.api.release((Message*)handle); }

TunnelPeer b_state(void*, uint64_t peer) {
    Identity who = identity_of(peer);
    switch (g.api.info(g.api.messages, &who, nullptr, nullptr)) {
        case kState_Connecting:
        case kState_FindingRoute:   return TunnelPeer::Connecting;
        case kState_Connected:      return TunnelPeer::Up;
        case kState_ClosedByPeer:
        case kState_ProblemLocally: return TunnelPeer::Dead;
        default:                    return TunnelPeer::None;
    }
}

void b_close(void*, uint64_t peer) {
    Identity who = identity_of(peer);
    g.api.close(g.api.messages, &who);
}

void b_log(void*, const char* line) { log_line("STEAM", "%s", line); }

TunnelBackend backend() {
    TunnelBackend b;
    b.send    = b_send;
    b.recv    = b_recv;
    b.release = b_release;
    b.state   = b_state;
    b.close   = b_close;
    b.log     = b_log;
    return b;
}

} // namespace

// ---------------------------------------------------------------------------

bool steamlink_host(uint16_t port) {
    g.error[0] = 0;
    if (!resolve()) return false;
    arm_requests();
    g.tunnel = tunnel_start_host(backend(), port);
    if (!g.tunnel) { set_error("could not start the relay tunnel"); disarm_requests(); return false; }
    log_line("STEAM", "hosting through the relay; friends join with %llu",
             (unsigned long long)g.api.self_id(g.api.user));
    return true;
}

bool steamlink_join(const char* addr, uint16_t* out_port) {
    g.error[0] = 0;
    if (!resolve()) return false;

    char* end = nullptr;
    errno = 0;
    const uint64_t host = _strtoui64(addr ? addr : "", &end, 10);
    if (errno || !host || !end || *end) {
        set_error("addr must be the host's SteamID64, the 17-digit number on their panel");
        return false;
    }
    if (host == g.api.self_id(g.api.user)) {
        set_error("that is this account's own SteamID -- paste the host's");
        return false;
    }

    g.tunnel = tunnel_start_client(backend(), host, out_port);
    if (!g.tunnel) { set_error("could not start the relay tunnel"); return false; }
    log_line("STEAM", "dialling %llu through the relay", (unsigned long long)host);
    return true;
}

void steamlink_stop() {
    if (!g.tunnel && !g_request_armed) return;
    tunnel_stop(g.tunnel);
    g.tunnel = nullptr;
    disarm_requests();
    log_line("STEAM", "relay closed");
}

uint64_t steamlink_self_id() {
    if (!g.api.ok) {
        // Polled from the panel every frame, so a Steam that is not up must cost
        // one lookup rather than one error in the log per frame.
        if (!GetModuleHandleW(L"steam_api64.dll")) return 0;
        static bool tried = false;
        if (tried) return 0;
        tried = true;
        if (!resolve()) return 0;
    }
    return g.api.self_id(g.api.user);
}

const char* steamlink_error() { return g.error; }

} // namespace mgmp
