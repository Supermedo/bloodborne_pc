// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void SetRip(void* ctx, void* rip);

void IncrementRip(void* ctx, u64 length);

/// Zero a 64-bit GP register. `which` is 0=RAX … 7=RDI, 8=R8 … 15=R15.
void ZeroHostGpr(void* ctx, u32 which);

bool IsWriteError(void* ctx);

} // namespace Common