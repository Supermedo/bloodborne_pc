# Mouse camera response: 2026-10-08

## Evidence

The supplied `mouse-camera-20261008-172508-42260.zip` contains 4096 camera-update
samples over 34.389 seconds, plus the original camera routine and bounded callees.
It was recorded on Camera Fix / Crash Diagnostics.

Raw mouse counts reached the angle fields immediately. Across eligible consecutive
mouse-owned samples, the next entry angles matched the preceding post-input angles;
there were no angle-feedback mismatches above `1e-6` radians. Input accumulation
therefore did not explain the reported reversal delay and vertical inertia.

The position-derived world-orbit direction lagged behind those angles. For example,
the pitch target stayed at approximately 1.15111 radians from 24.639 to 25.281
seconds while the orbit pitch continued from approximately 0.41109 to 1.02105
without vertical mouse input. These values are derived from position/focus vectors,
not a captured view matrix.

## Cause

The game performs another interpolation after computing its desired orbit position:

```text
factor = chase + (1 - chase) * camera.blend_override
position = previous_position + (desired_position - previous_position) * factor
```

The recorded camera has horizontal angular thresholds of approximately 10 and 20
degrees (`+1bc/+1c0`). This stage can hold horizontal response inside its angular
band, including after a reversal. Normal X/Z chase is 0.1 (`+1ac`), while Y chase
is 0.03 (`+1b8`). The unequal chase rates explain why equal raw angular gain still
felt different on the two axes, and why the camera kept moving after input stopped.

The scalar at `+130` is the blend override in this equation, rather than residual
angular velocity. The previous hook cleared it on each mouse update, leaving the
native chase active. Synchronizing angle fields alone could not remove that stage.

The routine also contains two `VEXTRACTPS` yaw stores and five immediate
angle/correction writes that the previous scalar-store discovery did not cover.

## Change

A permanent, ownership-gated hook at image offset `0x143e7e6` supplies `1.0` as the
local blend operand while the mouse owns free rotation. The equation then reaches
the desired orbit immediately on X/Y/Z. Native `+130` state and its writes remain
intact. The hook executes the original operand load for stick ownership, lock-on
or capture off.

The installer verifies the entire 37-byte equation before changing any code.
Angle discovery now decodes complete instructions and gates scalar, extract and
immediate stores to the allowed fields. Existing auto-rotation NOP patches are
preserved. Collision queries and subsequent radial-distance code are unchanged.
The new orbit gate is inline assembly with no additional native callback,
allocation, worker or file operation per update.

## Verification and limits

`mouse-camera-test` covers angle writes, all store forms, entry timing, atomic input
transport, native controller handoff, binoculars, signature rejection, concurrent
toggles and register preservation. `mouse-orbit-test` executes the position blend
and checks its output vector, including native versus mouse ownership.

The same orbit test accepted the actual captured camera routine and replayed the
report's input and angle state through the isolated operator: 3825 eligible
mouse-owned updates, 909 idle updates and 86 horizontal reversals all reached their
target vector immediately. Separate cases cover one-count input, 30/60/120/240
updates per second, native unequal chase rates, pitch limits, yaw wrap, camera
radii and vertical-stick/lock-on/F4 handoff. An instruction-scope comparison
confirms that collision, radial-distance and native override instructions remain
unchanged.

This is not a full gameplay replay: the test supplies a fixed focus/radius and
does not execute level geometry, physics, presentation or the complete camera
routine. Free rotation near walls, movement, binoculars, scripted transitions and
mouse/controller switching still need confirmation in the game. The report and
captured game routine are local verification inputs, not repository/package assets.

## Movement report and rejected quantization change

The later `mouse-camera-20261008-190629-15948.zip` contains 4523 updates over
37.684 seconds, including held W, W+A and W+D with F4 enabled and disabled.
The user subsequently identified adding A as the problematic transition;
adding D appeared normal.

The final pad input remains stable throughout each held combination. There are
no unintended button presses, neutral samples or right-stick commands. While
the mouse owns rotation, the next entry angles match the preceding post-input
angles; view yaw agrees with the stored yaw within approximately `7e-7` radians.
The capture does not show angle feedback or ownership changes causing the jerks.

The original input values and the subsequent change were:

| Input | Original report X/Y | Rejected change X/Y | Commit 302e4db X/Y |
| --- | --- | --- | --- |
| W | 128 / 0 | 128 / 0 | 128 / 0 |
| W+A | 37 / 37 | 38 / 38 | 38 / 37 |
| W+D | 218 / 37 | 218 / 38 | 218 / 37 |

Commit `7c6d2e8` rounded both diagonal components toward zero to keep their offset
inside a radius-128 circle. Its isolated input tests passed, but the user rejected
the gameplay result: W+A still jerked and W+D started doing so too. The jerks
continue while held; physical controller movement remains normal with F4 enabled.
That result invalidates the proposed fix. A radius check alone does not establish
the cause of the gameplay problem. Physical controller values can also round
outside this mathematical circle and must remain untouched.

The rejected change reduced both diagonal offsets to `(90,90)`: length 127.279,
approximately 99.44 percent of radius 128, and exactly 45 degrees. The original
W+D offset `(90,-91)` has length 127.988, approximately 99.99 percent, and an angle
of approximately 44.683 degrees from forward. The original W+A offset `(-91,-91)`
was exactly 45 degrees. The captured routine does not include character animation
selection; an unstable movement-sector boundary at exactly 45 degrees is a
hypothesis, not a verified game-code finding.

## Vertical-priority mapping and gameplay feedback

Commit `302e4db` restored the W+D sample that the user reported as working and
mirrors it around neutral X=128 for W+A. Normalization considers the neighboring
integer coordinates and selects the nearest candidate inside the radius-128
circle. Equal-error choices prefer the vertical component. All keyboard diagonals
therefore use horizontal offsets of 90 and vertical offsets of 91, preserving
near-full strength without placing the input at exactly 45 degrees. Cardinals,
opposing-key cancellation and mixed controller axes retain their existing behavior.
There is no temporal filtering or change to the working raw mouse camera hooks.

The input regression requires the recorded W+D `(218,37)` to remain unchanged,
W+A to be its mirror `(38,37)`, and diagonal squared magnitude to be at least 16380
and at most 16384. It also checks all 16 WASD states, repeated held-direction
transitions, remapped keys/mouse buttons, physical controller passthrough and
983040 mixed keyboard/controller states. This rejects both previous mappings.
These checks validate delivered input, not character motion or animation.

The user confirmed smooth movement when starting with W or S and then adding
A or D. Jerks remain in the reverse order: starting with A or D and adding W or S.
A fixed vertical preference therefore handles only one starting axis. A vector
with offsets `(90,91)` turns approximately 44.683 degrees from vertical but
45.317 degrees from horizontal. The same rounding choice cannot favor both
starting directions.

## Held-axis preference and validation limits

Commit `a9b9911` remembers the resolved digital direction at each pad read.
Adding an orthogonal direction retains the already-held axis as primary. That
axis wins equal-error integer rounding choices, giving `(90,91)` offsets for a
vertical start and `(91,90)` for a horizontal start. Both have squared magnitude
16381 and turn approximately 44.683 degrees from their respective starting axis.
Holding the combination does not alternate between the two vectors.

Releasing or cancelling the primary direction transfers preference to the
remaining axis. Releasing all movement actions, menu capture and a new pad session
clear the state. If both axes first appear in the same pad read, the deterministic
fallback remains vertical. This operates on resolved movement actions, including
remapped keys and mouse buttons. It adds no timer, interpolation or input delay;
raw mouse camera hooks and controller-only samples remain unchanged.

The regression covers all eight ordered cardinal-to-diagonal transitions,
240 consecutive held samples per order, releasing/re-adding the primary key,
secondary reversal, opposite-key cancellation, remapped/mouse-bound actions and
menu/pad-session resets. The old mapper fails the horizontal-first assertion.
Exhaustive bounds checks cover 1966080 mixed keyboard/controller samples across
both starting-axis histories. These checks validate input values and stability;
they do not execute character animation or level physics.

The user confirmed that adding a second direction now works in both axis orders.
A jerk remains when releasing the first key and continuing along the other axis,
for example W -> W+D -> D or D -> D+W -> W.

## Primary-key release handoff, 2026-10-09

The remaining transition changes direction by approximately 45.317 degrees in one
pad sample. Its angle is slightly larger than the successful 44.683-degree entry
into a diagonal. A single rounded diagonal cannot put both transitions below
45 degrees. The character's animation-sector implementation is still unavailable;
the input angle is verified, while its connection to the gameplay jerk is a
hypothesis supported by the preceding feedback.

On primary-key release, the mapper briefly swaps the diagonal's component priority
toward the remaining axis before returning to its full cardinal value:

| W -> W+D -> D | X/Y delivered to the game |
| --- | --- |
| W | 128 / 0 |
| W+D | 218 / 37 |
| Release W: handoff toward D | 219 / 38 |
| D after handoff | 255 / 128 |

This splits the release turn into approximately 0.633 and 44.683 degrees. Both
diagonal samples have identical squared magnitude 16381, so this step does not
reduce stick strength or insert neutral input. The same rule handles all eight
ordered orthogonal pairs, including horizontal-first movement.

The handoff expires on the first pad poll at least 8000 microseconds after release,
using the existing monotonic input timestamp. Extra polls within that interval
retain the same intermediate sample. This introduces a short directional delay
only when releasing the primary key; it adds no general movement filter. Releasing
the secondary key returns to the original cardinal immediately. Releasing all
movement keys, changing direction, opposing-key cancellation, menu capture or
pad reopen cancels the handoff immediately. Without a timestamp, it lasts one
poll; a backwards clock also expires it. Controller-only input and raw mouse
camera code remain unchanged. No extra clock call, worker or allocation is needed.

The previous mapper fails the new primary-release angle regression. The updated
test checks all eight orders at 30/60/90/120 input updates per second, repeated
polls during the handoff, the exact expiry boundary, held-state stability,
immediate stops, new directions, opposing keys, remapped mouse/keyboard actions,
native-axis return and menu/session resets. The existing 1966080 mixed-input
bounds cases also pass. These checks validate delivered input; gameplay confirmation
of the release correction is still needed.

If another report is needed, include W -> W+D -> D and D -> D+W -> W, their
mirrors, and physical-controller movement in the same open area. Enable F4 and
establish mouse ownership first; keep the right stick neutral. Private reports
and captured game code are not repository or package assets.
