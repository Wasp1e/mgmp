# House cat movement investigation

## Host-owned preview

`tools/restart_test.ps1 -HouseCatSync -HouseDragSync -SplitPrimary` starts
the opt-in experiment on both peers. The host simulates daytime cat movement
and sends position, facing, the base pose, three short idle actions and the
`jumpStart` crouch. The client follows fresh samples for a cat in the same room
or yard. It plays the crouch on the same 220 ms playback timeline as movement
with an empty animation callback. The host drives takeoff and the jump path.
Local pickups, replicated drags and nighttime still use their own paths.

House cat synchronization was verified in a two-player session on protocol 104.
Social actions and animations with native callbacks still need separate
validation. The older `-HouseWalk` experiment is still available for comparison.

The follower also skips HouseCat's second update (vtable `+0x58`, RVA
`0x1FD8B0`) while a fresh host snapshot controls the cat. That phase scans
nearby cats and can queue social reactions; its `HouseCat+0xD9` gate is reset
by the main update, which followers skip. Letting the second phase run alone
can repeat reactions as cats cross. The guard is build-checked; the reported
client-only swatting still needs a live retest with the new DLL. Host social
actions remain outside the supported replay set.

## Earlier experiment

Visible drag work is tracked separately in [house-cat-drag.md](house-cat-drag.md).
Independent walking/idle-action synchronization remains disabled during drag
testing.

These are observations for the Mewgenics build targeted by this project, not
portable offsets. Recheck them against the executable after a game update.
The experimental `debug.house_walk_sync` mode is disabled by default; it
now tries to copy the observed states 4–7, movement targets and positions.
It is unvalidated and does **not** synchronize all behaviors or animations.

## Identified code

MSVC RTTI identifies the `HouseCat` vtable at RVA `0xEFD038`. Its slot 10
points to RVA `0x1FACC0`, a large update function. Its state dispatch reads
the 32-bit value at `HouseCat+0x94`; values 0 through 7 have branches.

The branch for state 7 calls the function at RVA `0x1FE7D0`. That function
reads two **32-bit integers**, at `HouseCat+0xF0` and `+0xF4`, and produces a
world-space target. When `HouseCat+0xE8` is non-null, it incorporates the
holder's transform. Do not treat `+0xF0` as a pointer or compare the target
directly with world coordinates without accounting for that transform.

The state 7 branch uses the target and the cat transform at `+0x40` to
calculate movement. It may trigger a `jumpStart` animation, and it also
checks the holder; it is not safe to force this state without considering
the room, target validity and jumping.

The update contains references to `scratching`, `cleanSelf`, `stretch`,
`licking`, `playfulRoll`, `buttWiggle` and `hissing`. These are evidence that
behavior and animation interact, but do not by themselves identify every
state or prove that copying a state reproduces its animation. The update
also uses the presentation random stream at TLS `+0x198`.

The animation object at `HouseCat+0x68` exposes a base animation string at
`+0x150` and an active action string at `+0x190` when byte `+0x210` is set.
Read-only captures observed `standingIdle`, `sittingIdle`, `layingIdle`,
`sleepingIdle`, `walk`, `jumpUp` and `jumpDown` as base animations, and
`scratching`, `cleanSelf`, `stretch`, `buttWiggle`, `jumpStart`, `jumpLand`
and pose transitions as active actions. These strings are not sufficient to
recreate the game's animation callbacks safely.

In one experimental capture the client showed `jumpLand` for the same cat in
108 half-second samples while the host's observed landings lasted only a few
samples. It is not proven that state synchronization caused this, but the
experimental sync now avoids changing cats with active or queued animations
on either side. This guard needs live validation.

In a subsequent local capture with that guard, neither peer showed a sampled
`jumpLand` lasting more than 0.5 seconds. This is encouraging but does not
prove the guard fixes the earlier occurrence. The two games still had
different behavior states and positions: 2,131 of 7,280 approximately
time-aligned, same-room position pairs differed by over five world units.
Samples are correlated in time and must not be treated as independent trials.
State 2, including `standingIdle` and short random actions, is not currently
fully replayed by the experimental sync. The development version now tries
aligning state 2 with stationary states 4–6 when neither peer is animating;
it does not force a walking cat into idle. This remains unvalidated.

## Short actions: call-site evidence

The 2026-11-17 capture read these active names from `+0x190` while `+0x210`
was set: `scratching`, `cleanSelf`, `stretch`, `buttWiggle`, `playfulRoll`,
`swat`, and transitions including `jumpStart` and `jumpLand`.

In the update's random-action paths at RVAs `0x1FC499`, `0x1FC5E1`,
`0x1FC6E9` and `0x1FCCF4`, the game calls RVA `0x9AA5B0` with:

- `rcx`: the animation object from `HouseCat+0x68`;
- `rdx`: a game-owned `std::string` selected from an action list;
- `r8`: a game-owned `std::string` containing `_DEFAULT`;
- `r9`: an empty function object (its function pointer is null);
- stack arguments 5 and 6: zero.

The function consumes/destroys its string and callback arguments, so it must
not be passed borrowed remote buffers or ordinary literals as string objects.
Its code queues the two names at animation-object offsets `+0x220` and `+0x240`
and uses callbacks. Other animation paths, especially jumping, can supply
non-empty callbacks and must not be treated as interchangeable with these
random-action calls.

The opt-in `debug.house_action_sync` experiment attempts this call for just
`scratching`, `cleanSelf` and `stretch`, with inline strings and an empty
callback. It is not enabled by default. It only plays an action if the client
cat is in the same room and state and has no action in progress. The client
still independently chooses actions; samples can miss a short action, and a
busy client drops rather than queues the host's action. Matching a sampled
action name does not prove matching timing. This path requires live
validation, especially of game string and callback ownership.

In a 2026-11-17 local test, the host logged 72 selected actions and the client
logged 43 successful replay calls; no replay failure was logged. This does not
prove that the action completed or that the remaining 29 all failed: the
client deliberately skips actions when its cat is busy, in another state or
room, or too late. In that capture, 1,842 of 7,123 approximately aligned
same-room position pairs differed by more than five world units. The earlier
capture used a different session, so its percentage is not a controlled A/B
comparison. The player reported that positions seemed to diverge more.

## Measurements

With position synchronization **off**, a local two-instance capture on
2026-11-17 recorded cats every two seconds:

- In one host capture, states 4, 5 and 6 produced no measurable position
  change across 1,695 consecutive pairs where the cat stayed in the same
  room and state. State 7 moved in 167 of 295 such pairs.
- A later host capture, restricted to pairs with the same state and target,
  saw all 58 state-7 pairs move; the sampled `+0x38` object's `+0x48` value
  followed horizontal movement direction.
- Sampling is too sparse to classify every transition. The figures establish
  correlations, not that the fields can be written safely or that the same
  state implies the same animation.

The source instrumentation is opt-in (`debug.house_positions`); use
`tools/restart_test.ps1 -HousePositions` for a local capture. It reads these
fields and does not modify them.

## Before applying a synchronized state

1. Identify the animation invocation and completion paths, including the
   callbacks and fields used by state 2's random actions.
2. Validate room-relative target coordinates, path constraints, jump state,
   and the physics component's lifetime.
3. Determine which timer and flags must accompany each transition. Never copy
   raw pointers between processes or force behavior while a player drags a
   cat, or while it enters the pipe or box.
4. Test state transitions, all observed idle actions, walking, jumping,
   dragging from both peers, reconnect and night resolution against a backed
   up save before making the feature the default.
