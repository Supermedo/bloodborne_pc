# Raw mouse camera on Windows

Free-camera mouse input now bypasses the game's orbit-position interpolation.
The camera report from 2026-10-08 identified a horizontal angular hold band and
different horizontal/vertical chase rates after the angle update. The fix removes
that extra response stage while the mouse owns free rotation. See the
[analysis and verification](mouse-camera-analysis-2026-10-08.md).
Full gameplay validation of this build remains pending.

**F4** toggles capture. The launcher Controls page and in-game menu expose sensitivity
(1-400%, default 100%) and invert Y. At 100%, **900 raw counts = one radian on either
axis**. Binoculars use half sensitivity. Menus, name entry and loss of focus release
capture and discard queued motion; lock-on uses the game's camera.

## Mouse and controller ownership

Capturing the mouse does not disable the controller. Nonzero mouse movement claims
free-camera rotation. Moving the right stick returns both axes to the original game
camera, even with F4 enabled. While the stick is held, mouse deltas are discarded.
After release, the game retains ownership until the next nonzero mouse movement,
so the controller's normal behavior and release response are preserved.

The final pad axes are published after keyboard bindings, scripted input and replay
have been applied. Keyboard look bindings therefore work too. Values within eight
8-bit axis units of 128 are treated as resting-stick noise for ownership only;
**the axes delivered to the game are unchanged**. Both vertical-only and
horizontal-only deflections trigger the same handoff.

## Camera update

The native callback now runs at the **entry of the camera update**, before the game
reads either axis. The previous callback was attached to two pitch loads partway
through the routine; any earlier angle calculations could still use old values.

While the mouse owns the camera, each update:

1. Consumes bounded, accumulated raw counts with the same constant scale for X and Y.
2. Updates current pitch/yaw and clamps pitch to the supported range.
3. Synchronizes current angles (`+140/+144`), target angles (`+148/+14c`), the pitch
   copy (`+150`) and automatic follow targets (`+26c/+270`).
4. Clears the residual yaw correction at `+294`, leaving the native blend-override
   field at `+130` intact.
5. Supplies a local unity override to the final orbit-position blend. Its formula
   `k + (1-k) * override` becomes one on all three position axes, so the desired
   orbit is reached on that update, including immediately after a reversal.

Synchronization also runs on **idle updates**. Skipping stores alone left old
values in the target/correction fields; those values could still feed the game's
subsequent calculations. The normal follow camera and binocular target angles are
kept consistent when binocular mode is active.

The angle-store gates apply only while the mouse owns free rotation. Capture off,
controller ownership and lock-on execute the original instructions. The hook does
not change the controller axes, the game's camera-speed settings or presentation
queue. Collision queries and radial-distance adjustment retain their original
instructions. The local blend override also yields to controller ownership,
lock-on and capture off; it does not replace camera parameters globally.

Camera layout references: the [1.09 follow-camera metadata](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/include/bbhost/engine/sprj/camera.hpp),
[BBShadPS4Mouse instruction targets](https://github.com/ajungg/BBShadPS4Mouse/blob/89ca38a30bf2c17645b5618d784a541e74e0885f/src/main.cpp#L209)
and [BBMCC's validator](https://github.com/Mob100percent/BBMCC/blob/819046cdbddd9d63460c7a4169e374f600267b35/src/Main.cpp#L440).
These establish field and instruction locations; the native hooks and input
transport here are independently implemented.

## Raw input

[SDL's Windows relative backend](https://github.com/libsdl-org/SDL/blob/release-3.4.0/src/video/windows/SDL_windowsmouse.c#L677)
uses native Raw Input. Before SDL initialization the port overrides inherited hints:
system acceleration off, relative speed scaling one, warp motion off and warp
emulation off. It consumes device deltas from the game window, not cursor positions
or virtual stick values. Synthetic touch/pen mouse events are ignored.

Sensitivity has no velocity curve, dead zone, frame-time multiplier or smoothing
filter. There is no polling worker: the window thread accumulates motion and the
guest camera thread consumes it during its own update. Both axes share one lock-free
atomic accumulator, so a diagonal packet cannot be split across two updates.
Native calls preserve
integer registers, flags, the SysV red zone and complete OS-enabled XSAVE state,
including AVX upper halves and MXCSR.

## Hook validation

Installation requires the matching Bloodborne 1.09 routine. Before changing any code,
the loader checks:

- The existing 18-byte pitch-load signature and four auto-rotation anchors.
- The complete 37-byte orbit-blend equation before installing its nine-byte hook.
- A position-independent entry prologue, decoded into complete instructions, and
  the routine's `mov r13,rdi` camera-argument setup.
- The routine's 29 original scalar-store signatures to its five main rotation
  fields; additional target/convergence stores are decoded separately.
- Two `VEXTRACTPS` angle stores and five immediate angle/correction stores.
- Each relocated store's size, camera base, register/immediate and allowed field.

Discovery decodes the whole routine on instruction boundaries. Scalar stores,
extracts and immediate writes to angle/target fields share the ownership gate;
native writes to `+130` remain active.

Only verified register/stack prologue instructions are copied; branches and
RIP-relative instructions are rejected. Existing nine-byte NOPs from **Disable
Camera Auto Rotation via Movement** are preserved. Mismatches reject the complete
installation and log the offending bytes. F4 changes atomic gates; it never
rewrites executing instructions. No guest camera pointer is retained by another
thread. Notifications wrap within the game viewport.

## Validation

`mouse-camera-test` executes the generated entry and store hooks against synthetic
guest instructions, including a yaw read **before** the old pitch-load hook. It checks:

- Equal X/Y angles on the same update, packet sizes from 1 to 900 counts and equal
  totals at 30/60/120/240 camera updates per second.
- Concurrent diagonal-packet production and consumption without axis tearing.
- Stale targets and nonzero correction state seeded before 120 idle updates.
- Capture enabled without mouse motion, vertical/horizontal/diagonal stick handoff,
  held-stick priority, neutral release and mouse reacquisition without queued jumps.
- Every discovered store under mouse, controller, capture-off and lock-on ownership.
- Binocular angle synchronization and idle behavior, inverted Y and pitch limits.
- Unsupported signatures/prologues, partial NOP patches and complete rollback.
- Flags, MXCSR, volatile registers, the red zone and AVX state preservation.
- Concurrent F4 changes without executable-memory rewrites.

`pad-test` checks that a virtual SDL controller's actual final X/Y values reach the
ownership bridge, including vertical-only movement and neutral menu capture.
`raw-mouse-test` creates a real Win32/SDL window, checks Raw Input registration and
hint overrides, and passes both SDL events and native `SendInput` packets through
the production event pump. It also checks equal-axis gain and synthetic-device
filtering. It skips when desktop focus is unavailable and does not alter Windows
pointer settings. Overlay tests cover settings clicks and notification layout.

`mouse-orbit-test` executes the orbit-position equation with the actual generated
gate and observes its output vector. It checks one-count reversals, stops,
30/60/120/240 update rates, unequal native chase rates, angle/radius limits and
controller/lock-on/toggle handoff. It also verifies that other collision, distance
and native blend-override instructions have not been patched.

An optional local replay accepts `code.txt` and `state.bin` from a camera report:

```text
mouse-orbit-test.exe path/to/code.txt path/to/state.bin
```

On the supplied report, the production installer accepted the captured routine.
The isolated orbit operator reached the target on all 3825 eligible mouse-owned
updates, including 909 idle updates and 86 horizontal direction reversals.
This replay uses recorded input/angle state with a fixed focus/radius; it does
not simulate level geometry or execute the entire game update. Captured game
code and private report data are not distributed with the tests.

**Gameplay validation is still needed:** no complete game dump is available in
this workspace. Check
free-camera flicks and stops, equal horizontal/vertical sweeps, right-stick movement
with F4 on, controller-to-mouse transitions, lock-on, binoculars, menus and Alt+Tab.
Render rate, queued frames and frame generation can still affect display latency.

## Capture a camera report

Run **Trace Mouse Camera.cmd** from a diagnostic package. It uses the game and
settings already selected in the launcher. Load a character, enable F4, then spend
about 15 seconds reversing left/right, sweeping up/down and stopping completely.
Close the game. The script creates **logs/mouse-camera-*.zip** for this session.
Python installation is not required. Starting the game normally leaves tracing off.

For sustained movement jerks, run **Trace Keyboard Movement.cmd**. After loading,
enable F4 and move the mouse once to start recording. In an open area, hold W for
3 seconds, W+A for 5 seconds and W+D for 5 seconds with the mouse still. Repeat
while looking around, then turn F4 off and repeat the movement combinations.
Also compare the reverse key order: start with A or D, then add W or S and hold
each combination for about 3 seconds.
While holding a diagonal, release the first key and keep the second held for
3 seconds: W -> W+D -> D, then D -> D+W -> W. Repeat with A and S.
Close the game to collect the ZIP. This procedure captures both sides of the F4
comparison; collecting a report does not fix movement.

For a controller comparison, enable F4 and move the mouse once before repeating
the directions with the physical left stick. Keep the right stick neutral so the
same mouse-owned camera path remains active for both input sources.

The report includes the raw X/Y counts, sensitivity, ownership flags, camera angles,
follow parameters, position/reference/focus vectors and the unmodified camera
routine with bounded direct callees and referenced constants. `before_*` fields
are sampled at update entry; position then reflects the preceding game update.
`after_*` fields follow this port's mouse writes, before the game continues. Separate
camera-object addresses must be analyzed separately. This makes it possible to
distinguish input loss, angle feedback and subsequent camera-position chasing.

V2 reports additionally record the final pad buttons and all four axes as one
atomic snapshot, the view and character bases, follow vectors and native camera
flags/timers. Pad values include remapping, scripted input and replay. They are
the most recent complete pad sample, not a new poll from the camera thread.
The metadata counts diagonal samples with capture on/off and mouse ownership.
The exporter continues to read older V1 reports.

Recording starts at the first mouse movement and stops after 16384 camera updates.
The fixed V2 file is approximately 8.63 MiB. Startup creates and prefaults the mapping;
camera callbacks perform bounded memory copies, with no allocation, file API calls
or logger locks. Original code is disassembled once before the hooks are installed.
No assets, saves, player names or settings are collected.
Tracing is optional and does not change camera behavior. The direct orbit fix
also runs during normal launches with tracing disabled.
