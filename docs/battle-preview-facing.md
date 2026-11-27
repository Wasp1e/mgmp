# Battle sprite and CTRL facing

Reported 2026-11-27: after AI turns, the sprite sometimes faces a different
direction from the CTRL indicator, on both host and client.

The mod's UpdateDecision guard restored Character+0x388 and +0x390 after the
preview. Character::Face also updates the Animator, animation suffixes and
turn callbacks, which that rollback did not undo. This provides a mod-side
mechanism for the reported symptom; occurrence in the unmodified game has
not been established.

Verified directly against the installed executable:

- Brain::UpdateDecision RVA 0x137EF0 calls Face at 0x1381E9.
- Character::Face RVA 0x10CE50 receives the packed direction in RDX and flags
  in R8B/R9B. Its preview call sets both flags to zero.
- Face changes animation state as well as the combat facing field.

The FACE detour blocks only that direct call while the existing lockstep
preview guard is active for the same character. It matches the argument
instructions and relative call destination, allowing action/status calls
inside UpdateDecision to pass through. Outside a battle session it passes
through. No vtable slots or network messages were added.

The new target signature is unique in the installed PE. test_previewface
checks rebasing and rejection of unrelated callsites; test_resolve_live also
checks that the real executable contains exactly one matching preview call.

## Follow-up after unsuccessful live validation

The user reported that the preview-call guard did not eliminate the symptom.
The new host log (`mgmp_trace_20261127-202358.log`) confirms FACE was installed
and the preview call was blocked. The first change was therefore insufficient.

Read-only inspection of that running game found a live character at (3,9)
with current/target facing (0,1), art scale (1,1), Animator flip -1 and suffix
`F`. Face's ordinary branch expects suffix `B` for that direction. This
confirms stale visual state independently of which path originally caused it;
the exact writer responsible has not been established.

A second change refreshes mismatching ordinary sprites at turn boundaries,
after owner-facing pinning, walking current membership including new summons.
It calls the verified Face function only with +4CC/+4CD off, no locked or
rotated direction, a cardinal current direction, an enabled live character,
and no pending turn animation. Both current and desired facing are preserved.
Special rendering modes are deliberately skipped. Calls and corrections are
logged under SPRITEFACE. This currently requires the pinned game build.

test_spriteface compiles the real repair function with a substituted native
Face. It checks stale sprite correction, preservation of Character memory,
idempotence and exclusions. It does not execute the game's animation engine.

## Follow-up: retry after animations settle

The second change also proved incomplete. The host log
`mgmp_trace_20261127-211004.log` shows the sprite repair running, including for
character 0x29FD1FB5308 at turn 39. At turn 43 that same PatternBrain moved
from (7,9) to (2,9) and committed facing (-1,0). Read-only inspection during
the following player's turn found current/target (-1,0), Animator desired and
cached flip +1, desired and cached suffix B, and root scale +1. All ordinary
repair preconditions were satisfied at the time of inspection. The old repair
had no further opportunity to run between turn boundaries.

Idle retries now run at most every 100 ms from the game-thread pump. They
require a recently updating TurnControl with the same live scene/list and skip
the current actor. Only the Animator pose `idle` is eligible; walk, attack and
pending turn flags still defer the repair. Current live membership includes
new summons. The existing native repair keeps combat fields unchanged.
Successful retries log `SPRITEFACE idle retry` with the character and facing.

The regression test now exercises a deferred turn, walk and attack, followed
by successful repair on idle without a new boundary. Exact animation-writer
attribution remains outstanding.

On November 28 the user reported that the latest game showed no enemy sprite
versus facing-indicator discrepancies and provisionally accepted the fix.
Continue broader checks after enemy/summon turns, movement, attacks and forced
facing effects. This is a live-play observation, not an exhaustive validation
of every animation mode.
