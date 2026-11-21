# Experimental visible House dragging

Protocol 92, **off by default**. Does not ship in the play build (protocol 84).
Keep `debug.house_walk_sync` and `debug.house_action_sync` off for these tests.

Run two local peers:

```powershell
tools/restart_test.ps1 -Force -BuildDir build_house_exp -HousePositions -HouseDragSync
```

`-HouseDrag` remains the old read-only network trace. `-HouseDragSync` enables
the actual visual/physics experiment; it does not need `-HouseDrag`.

## Implementation

- Samples the real local hand (`comps+0x4490`, held cat at `+0x38`) at about
  20 Hz, with a final sample before the ordinary drop. No process pointer is
  transmitted. Uses the existing diagnostic CHOICE kind 49.
- Incoming messages are queued and applied on a House component update, not
  from the network pump. Coordinates must be finite and within +/-10000.
- Leases last 1500 ms. Lowest peer id wins a same-cat conflict, independent
  of arrival order. Local input is never used as the remote cat's hand.
- Locally verifies cat key, pointer, allocation cookie and scene before
  touching a retained visual. Old scene references are discarded.
- Detaches the receiver's cat only from a known safe room: `FurnitureGrid`
  (vtable `0xEFCDE8`) or `CatPlacementArea` (`0xEFCF10`). Pipes/boxes are not
  taken apart here; a box removal must arrive through the box's existing sync.
- While held, skips this cat's `HouseCat::update` (`0x1FACC0`) and
  `HouseCatPhysics::update` (`0x1FF3F0`), moves its transform, and zeros the
  velocity/history. Other cats continue updating normally.
- Changes its draw depth by the same +100000 used by local pickup. The game's
  consumed `_DEFAULT` string/animation command 8 supplies the held pose.
- Pipe/box add guards reject automatic pickup while the remote lease is
  shown. A real replicated placement ends the freeze before the normal add.
- In protocol 103 a confirmed room or yard drop retires the drag lease and
  resumes native physics from the exact release point. The receiver waits
  for its cat to land before accepting movement samples again, and the host
  pauses publishing samples for that cat during its own fall.
- In protocol 104 the receiving game cancels the old walk on pickup. The
  releasing game sends the velocity and tumble flag from its first native
  physics update so both games can show the same kind of fall.
- With `house_cat_sync` enabled, guests use the host's position and pose
  samples from release through landing when the host drops a cat. Playback
  starts at the confirmed release point and continues into ordinary movement.
- Waits up to 700 ms for a placement after an unpaired release. A lost lease,
  disconnect, or missing placement restores the original safe room and
  position. An older placement cannot interrupt a newer active pickup.
- Hook validation failure leaves visible dragging disabled.

## Local observations, 2026-11-19

Backed up the full game data before testing to:
`%LOCALAPPDATA%/Temp/opencode/mgmp-house-auto-20261119/saves-before-190254`.
Tests use the previously designated third campaign slot, without ending a
day or sending a cat to an NPC.

After correcting the initial room-type guard to recognize FurnitureGrid:

- Host/client logs `mgmp_host_20261119-191851.log` and
  `mgmp_client_20261119-191901.log` record visible pickups and ordinary
  placements in both directions.
- Screenshots show host-held Astolfo (`0x175`) moving on the client, and
  client-held Ivan (`0x14E`) moving on the host.
- A 2300-ms suspension of the test client caused a logged restore on the
  host at 19:26:27.772, and a fresh visible pickup after resume at 19:26:29.471.
  This tests missing updates, **not** full reconnect correctness.
- Hovered a held cat near the pipe and box and moved it back before release:
  no NPC screen or box insertion was observed. These observations alone do
  not prove the pointer reached every collection boundary.
- Initial screenshots showed different held poses (the receiver kept its
  previous idle animation). The explicit held-pose command was added after
  that observation and needs separate validation.

Screenshots are local under
`%LOCALAPPDATA%/Temp/opencode/mgmp-house-auto-20261119/`.
`tools/house_desktop_test.ps1` performs bounded, explicitly PID-targeted
game-only input/captures after user permission. A focus failure stops input.

## Still required

In-game simultaneous pickup with independent inputs (one physical desktop
mouse changed focus/released the first drag instead), full disconnect and
reconnect, box entry/removal, actual pipe delivery, scene departure, repeated
fast pickups, and WAN jitter. Unit tests cover priority/order/expiry but are
not evidence those in-game paths are safe. Do not promote this build yet.
