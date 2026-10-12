# Vulkan command-data crash and missing dump, 2026-10-09

The supplied `message (6).txt` ends with:

```text
GPU [Debug] <Critical> vk_scheduler.h:869 ReserveRecordData: Assertion Failed!
STOP: GPU library assertion failed (see GPU log above)
— игра завершилась (код 23) —
```

Log SHA-256: `9d39e3b86e417a78068347c9e0d24310f472927588d1dbe67967b52e0b099599`.
The report contains no exception address, stack trace or requested byte count.
It therefore identifies the failing capacity check, but not which resource,
copy-region list or barrier array caused that request. Missing optional game files
and earlier Vulkan format warnings do not establish the cause of this assertion.

## Cause and storage correction

`Scheduler::ReserveRecordData` asserted that every payload plus 1024 bytes of
command headroom fits a fixed 128 KiB `RecordChunk`. Variable-length arrays passed
to `RecordData`, including buffer-copy regions and image barriers, had no general
path for larger payloads. The assertion deliberately terminates the process; it is
different from the earlier guest-code access violation.

Normal commands and fitting payloads continue to use inline chunk storage.
An oversized reservation now allocates an uninitialized data block owned by that
same chunk. Nested `RecordData` calls consume the reserved block, including grouped
reservations whose individual arrays fit but whose total exceeds 128 KiB. Pointers
remain stable until all commands in the chunk have executed. The recording worker
then releases the extra blocks before recycling the chunk.

Command headroom, alignment and ordinary chunk rollover remain checked. Chunk
size accounting includes overflow data for scheduling decisions. Recording stays
on its workers with the original submission ordering; a large payload does not
switch the frame to synchronous recording. Small payloads introduce no new heap
allocation. The mouse and keyboard mapping are unchanged.

## Why no dump was written

The GPU assertion handler called `std::_Exit(23)` directly. That path raises no
Windows exception and therefore never reaches the exception-based minidump writer.
The absence of a dump in this report follows from that code path; the log does not
show a failed file write.

Before exiting, fatal GPU assertions now capture the current Windows context and
request a dump from the existing dedicated reporter thread. The minidump carries
the failing thread, modules, stack and reason `GPU library assertion failed`.
Exception marker `0xE0424201` explicitly denotes a port assertion; it is diagnostic
metadata, not a raised hardware exception. Exit code 23 remains unchanged.

The first failure owns the snapshot. Concurrent fatal threads wait on the same
manual-reset completion event, preventing context overwrite or process termination
while the file is being written. The wait remains bounded at 10 seconds. Existing
`BB_CRASH_DUMP=0` and `BB_CRASH_DIR` behavior still applies; an unwritable directory
reports the Windows error and preserves the original exit code.

## Verification and limits

The new `record-data-test` reproduced the original assertion at the same source
line before the storage change, without any game files. It submits 5718 buffer-copy
regions (137232 bytes), then overwrites/frees the caller's descriptor array.
GPU readback checks every copied value across 12 passes and chunk recycling.
Alternating passes exercise one oversized array and a grouped reservation with
two individually fitting arrays plus an aligned marker. The test passes with
asynchronous submission, synchronous submission and direct recording.

`gpu-assert-crash-dump-test` runs the production assertion handler on the main
thread, a worker and two concurrent workers. Each process exits with code 23 and
produces exactly one readable minidump containing its assertion marker, thread
and reason. Disabled dumping and failed writes also retain exit 23.
The existing access-violation dump regression still preserves its original
exception, distant guest callers and bounded nearby object pages.
The full native suite completed with 33 passing checks and one capability-based
skip for DLSS Frame Generation.

These tests verify the reported capacity failure and the missing-dump path.
The original gameplay session cannot be replayed from this log, so its exact
resource workload and full gameplay stability still need confirmation.
