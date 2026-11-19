# MoonHandFlail preview disconnect (2026-11-19)

Affected play build: protocol 84.

Evidence:

- Host log `build/Release/logs/mgmp_trace_20261119-170045.log`:
  at 17:57:38.155, cat 20 selects `MoonHandFlail` with target
  `(-4000, -4000)`; the host applies the ability and the hand throws the cat.
- Received friend log
  `build/Release/logs/friend/friend1_got175748_mgmp_trace_20261119-170046.log`:
  at 17:57:40.713, `undecodable AIM frame from peer 0, 43 bytes -- closing`.
  There is no received MoonHandFlail ACTION before the disconnect.
- Reconnect preserves the client's live fight, but catch-up replays old
  decisions, then halts on the previously used `cm_StrBuff` slot at turn 9.
  That replay problem is not repaired by this change.

`dec_aim` rejects targets outside [-1000, 1000]. The sender used to send the
PlayerBrain's off-grid selection sentinel unchanged, disconnecting the peer
over a cosmetic preview before the real action could arrive.

The sender now converts an unrepresentable AIM into an inactive, zeroed
preview for the same cat/battle. Gameplay ACTION messages are untouched.
Wire format and decoder bounds are unchanged; the fix works with protocol 84.
Both players should update because either player can select such an ability.

`test_proto` checks the observed sentinel, each target/direction bound and an
unchanged valid boundary preview. An in-game MoonHandFlail test is still needed.

This build also suppresses save-scum penalties/Steven as requested.
