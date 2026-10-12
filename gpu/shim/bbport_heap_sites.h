// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_HEAP_SITES=1: live allocations of the game's heap per call site (diagnostics).
#pragma once

namespace BbHeapSites {
/// Wraps the game's malloc replacement once libc.prx has set it up (BB_HEAP_SITES=1).
void Install();
/// Prints the call sites whose live bytes grew since the last report.
void Report();
/// The result of the game's release check (0xbf0790) seen at 0xbb5fb0.
void NoteReleaseCheck(unsigned result);
/// The counters alone (frame overlap leak watch): allocations minus frees since installed.
void InstallCounters();
bool Counting();
long long LiveAllocations();
} // namespace BbHeapSites
