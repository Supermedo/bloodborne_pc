# Silent termination and lost Windows status, 2026-10-09

The supplied `message (7).txt` ends with launcher status `4294967295`
(`0xffffffff`). Its SHA-256 is
`13c06b0022617cf926c1f687a21c700b3ddb8df6fb48a58544e3b3f2de3aba89`.
The native build timestamp is `6ac8d3cb`. There is no fault address, assertion,
dump-write failure or original native exit code in the report.

The last missing-file messages do not identify a crash. The report includes
mods and external patches, but contains no evidence that any particular one
caused termination. The underlying gameplay failure remains undetermined.

## Confirmed status propagation defect

Windows `subprocess` returns native process status as an unsigned DWORD.
The bundled Python 3.12 uses a signed C `long` for `SystemExit`; passing an
unsigned status above `0x7fffffff` overflows and produces `-1` instead.
This converts different native failures into the same launcher status.
[CPython's implementation](https://github.com/python/cpython/blob/3.12/Python/pythonrun.c)
uses `PyLong_AsLong` in `_Py_HandleSystemExit`.

The old frozen launcher reproduced the conversion for `0xc0000005` and
`0xc0000409`. These are test inputs, not inferred codes for this user's crash.
`0xffffffff` could also be an actual process termination status.

`run.py` now logs the native status before leaving, in decimal and hexadecimal,
and returns its signed 32-bit equivalent. The frozen script and `--play` roles
apply the same conversion. These paths preserve all 32 bits through Windows process exit.
Ordinary exit values remain intact, including successful exit and assertion 23.

## Capturing failures outside the failed process

The ordinary reporter still runs on its dedicated thread. It now reports
whether initialization succeeded, was disabled, or could not create its worker.
The process-wide unhandled filter is installed again after graphics runtime
initialization because DLLs can replace it.

Some terminal paths bypass in-process exception handling. Microsoft's
[RaiseFailFastException documentation](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception)
explicitly describes that behavior. The original log does not establish that
this particular failure was fail-fast.

`Trace Crash.cmd` starts an optional `bb-crash-monitor.exe` observer through
`BB_CRASH_MONITOR=1`. It launches the actual probe with
`DEBUG_ONLY_THIS_PROCESS`, passes ordinary first-chance exceptions to the
child's handlers, and captures the first unhandled second-chance exception.
It consumes only the debugger's initial breakpoint. GPU page-tracking faults
and handled application exceptions keep their normal handlers.

The observer supplies the child's actual thread context, exception record,
process and thread IDs to the existing dump writer. Memory collection reads
the child with `ReadProcessMemory`/`VirtualQueryEx`. Private executable pages
near PCs/callers and nearby objects remain limited to 2 MiB of additional pages;
the entire game image is not copied. The child's original exit status is retained.
Microsoft recommends using a [separate process for MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump)
when possible.

The observer is not part of ordinary launches. Debugger event processing adds
overhead during diagnostics, especially with frequent page-tracking faults.
The resulting `logs/crash-trace-*.zip` contains this session's log, dumps and
binary fingerprints. It excludes game assets, saves and settings files.
Captured helper output and diagnostic streams use UTF-8 explicitly, so a Unicode
path is preserved regardless of the Windows locale.

## Verification and remaining evidence

Native tests exercise `RaiseFailFastException` on the main thread and a worker,
the `__fastfail` instruction, and an unhandled access violation. Each retains its
original status and produces one readable dump of the child, with the actual
exception thread, modules and context. Handled exceptions continue and clean
exit produces no dump. Tests also cover Unicode paths, empty/quoted arguments,
disabled dumping and failed file writes. Existing in-process fault and concurrent
GPU assertion dump tests pass.

Frozen-package checks cover raw `SystemExit`, complete preparation/launch status
propagation, and report collection through the configured `--play` path.
The next report from `Trace Crash.cmd` is needed to identify and fix the original
gameplay crash. This change fixes status loss and adds capture of otherwise
unreported terminal exceptions; it does not establish a fix for that gameplay cause.
