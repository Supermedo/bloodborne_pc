# Gameplay crash on 2026-10-08

Status: the failing access is identified; the underlying guest-data failure is
**not fixed or reproduced in actual gameplay**. The accompanying runtime change
improves crash capture so a recurrence retains the missing evidence.

## Supplied evidence

| File | SHA-256 |
| --- | --- |
| `message (5).txt` | `dd05de7fc5bbee2eb9ab98b337a97ea4bc8216041d150d5349531c48325c9cf9` |
| `bb-crash-20261008-191525-39988.dmp` | `ed3d971f84f5b5889730b4d837c34290d8d33ad49e9951d59d7a2ecf43511dd4` |

The crash occurred on another computer during ordinary gameplay. Neither that
computer's prepared game image nor its live process is available for inspection.

The minidump's executable matches the packaged **v2 Raw Mouse** build:

- PE timestamp: `0x6ac7bbc7`; image size: `0x34e2000`.
- CodeView GUID: `74CB09A2-076E-AC5A-4C4C-44205044422E`, age 1.
- Matching archived executable SHA-256:
  `7cd69f87e65eccc6db1a87e648e68bef240c70b33479311b02b9d55504293309`.
- This predates the camera update-entry/handoff change in `b9acfab`.

The dump does not contain the entire executable, so its whole-file hash cannot
be recomputed independently from the dump. The PE identity and CodeView record
were compared to the archived binary.

## Confirmed failure

Process 39988, thread 39432, exception `0xc0000005` (read access violation):

```text
Guest base:      0x0000000800000000
Instruction:    0x000000080090202d  (guest +0x90202d)
Fault address:  0x0000000000000008
RAX:            2
RCX:            0
RBP:            0x17fd4f0

movslq  0x4(%r13), %rax
...
vmovss  0x10(%r13), %xmm0
movq    -0x80(%rbp), %rcx
vucomiss (%rcx,%rax,4), %xmm0
```

The float comparison reads `RCX + RAX * 4 = 8`. Its array pointer was loaded
from stack local `[RBP-0x80]`; that local is also zero in the captured stack.
The failing access is in guest code. The prior Vulkan command-dispatch crash
has a different location and call stack.

The recorded caller chain begins:

```text
0x90341f -> 0x8e0602 -> 0x8e1b07 -> 0x8c3fbc -> 0x8c8475
-> 0x1dcdf14 -> 0x1e2bceb -> 0x1549731 -> 0x193b37d
```

The `0x1dcdf14` caller lies near the independently documented entry of
`SprjHkAiManager`'s dynamic navigation-mesh update, `0x1dcdad0`.
[Pinned reverse-engineered symbol reference](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/include/bbhost/engine/symbols.hpp#L427).
This identifies the likely subsystem; it does not establish which object
owns the null array or why it became null. The supplied dump lacks the caller's
instructions needed to verify that mapping against this exact game image.

The rendering-stall message, missing optional map texture files, enabled mods,
and camera hooks are not sufficient evidence of causation. No mod, graphics
setting, frame cap or camera setting has been proved responsible.

## Why this dump cannot establish the cause

The normal minidump contains only 256 bytes of guest instructions around the
fault, plus another thread's current instruction window. It omits the caller
code and the heap data at the relevant registers, including `RBX`, `R13` and
`R15`. The instruction that originally supplied `[RBP-0x80]` is outside that
window. An uninitialized field, allocation failure and lifetime/race issue
cannot be distinguished from this evidence.

Skipping the instruction, fabricating an array, or catching and continuing
this access would leave a partially updated navigation mesh. No such recovery
or guest-code patch is included.

## Improved capture

Windows dumps now additionally select:

- Guest code around the fault and each of at most 32 callers (8 KiB on either
  side, rounded to page boundaries).
- Readable object pages addressed by registers and the first 16 KiB of stack
  locals, followed by one further pointer level.
- The virtual-memory map and a comment stream recording guest base/size and
  the capture budget.

Extra selected pages are limited to **2 MiB** and selection to **1.5 seconds**.
Selection uses preallocated storage and safe memory reads on the existing dump
worker. It adds no per-frame tracking. Guard/inaccessible pages are omitted;
optional read failures cannot replace the original exception. An extended-write
failure retries the ordinary minidump. The original exit code 3, ten-second
worker wait, Unicode paths and `BB_CRASH_DUMP=0` remain supported.

The implementation uses the documented DbgHelp
[memory callback and read-failure semantics](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/ne-minidumpapiset-minidump_callback_type).
It does not request a full-memory dump. Ordinary DbgHelp thread stacks/modules
and memory-map records are additional to the 2 MiB extra-page budget, so total
file size can be larger.

## Validation and next evidence

The native test reproduces the exact null float-array read at address 8 with
synthetic guest code. It verifies the original exception, a distant guest
caller, a root object, an indirect object field in the middle of a page,
the memory map and guest metadata. A dense pointer page exhausts the extra
budget while a corrupt frame pointer terminates the walk safely. Disabled and
failed dump writes retain exit code 3.

Full Windows validation passed: 27 native tests passed, one optional DLSS frame
generation availability test was skipped; 95 Python tests passed with 17
platform skips. These checks do not reproduce the game's navigation-mesh update.

To diagnose the existing crash, obtain the prepared `out\boot.bin` from the
computer that produced these files. To diagnose a recurrence, use the new
diagnostic build and retain its matching session log and `.dmp`. The diagnostic
build also includes the latest camera changes; their presence is not a claim
that they fix this crash. Actual gameplay validation remains outstanding.
