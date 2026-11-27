# Halt after TallBotSuplex, 2026-11-27

Report: build/Release/logs/reports/halt_20261127_220058_929_8cc29619b6d89426_14.
Battle 8cc29619b6d89426, hash turn 13, logged global turn 131.

Both peers agreed at turn 12 and executed TallBotSuplex with the same target
(6,7) and direction (1,0). Cat 19 moved to (4,7) and lost 3 HP on both peers.
Its affecting elements were 00300000 on host versus zero on client. The
authored ability has Stun and Immobile effects, each with chance .5. RNG also
differed. The client did not record a .mgr, so its per-roll inputs are absent.

An earlier health-only discrepancy at turn 7 was corrected by the existing
host-authoritative health path; turns 8 through 12 subsequently agreed. This
does not establish equality of every un-hashed passive/statistic.

## Confirmed instrumentation ABI defect

Host had debug.record enabled; client did not. In the installed PE,
randfloat (RVA 1595A0) changes XMM0/1 but leaves XMM2..5 untouched. The native
luck sampler rand2 keeps its fractional threshold, floor, unit and first
sample in those registers across calls to randfloat at 94F5AD and 94F716.
The former ordinary C++ hook called the recorder without preserving them.
The recorder and its callees are legally allowed to destroy these registers.

The replacement assembler entry calls the native leaf first, then preserves
its volatile integer and SIMD register outputs across the recording callback.
It preserves the returned random number, state, native XMM1 and XMM2..5.

test_rng_float executes an isolated relocated copy of the real native leaf,
compares all six SIMD registers and all four RNG words for 256 seed/scale
combinations, and exercises the real native luck sampler with a deliberately
clobbering observer. The old hook-shaped test fails register preservation
under that observer; the replacement preserves it.

## Limits of attribution

On this machine the real recorder did NOT reproduce an XMM2..5 overwrite in
the isolated test, and six native luck-sampler cases also agreed through the
old hook-shaped real-recorder path. Therefore the ABI defect is repaired,
but it is not proven to be the cause of this specific Halt. A repeat with
recordings from both peers is still required to distinguish differing roll
inputs, an earlier RNG divergence, and recorder interference. The Halt and
element comparisons remain enabled. No status/health synchronization workaround
was added to hide the discrepancy.
