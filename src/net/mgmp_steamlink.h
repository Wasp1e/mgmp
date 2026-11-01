// mgmp_steamlink.h -- reach a friend through Steam's relays, with no port forward.
//
// A TunnelBackend over ISteamNetworkingMessages. net.transport = "steam" in
// mgmp.json, or the box on the panel. net.addr is then the host's SteamID64
// rather than an IP.
//
// Everything is resolved at run time from the steam_api64.dll the game already
// loaded, so the mod takes no build dependency on the Steamworks SDK and uses the
// Steam session the game itself initialised, under the game's own app id.
#pragma once

#include <cstdint>

namespace mgmp {

// The host: `port` is the loopback port net_host is listening on. Only Steam
// friends are let in.
bool steamlink_host(uint16_t port);

// The client: `addr` is the host's SteamID64. On success *out_port is the
// loopback port to net_join.
bool steamlink_join(const char* addr, uint16_t* out_port);

void steamlink_stop();

// Our own SteamID64, or 0 if Steam is not reachable. For the player to copy and
// send to a friend.
uint64_t steamlink_self_id();

const char* steamlink_error();   // "" unless the last call above failed

} // namespace mgmp
