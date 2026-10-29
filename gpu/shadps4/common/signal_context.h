// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

// bbport: the registers the SRT walker's fault handler loads in place of a faulting read.
enum class Gpr { Rdi, R10 };
void SetGpr(void* ctx, Gpr reg, u64 value);

bool IsWriteError(void* ctx);

} // namespace Common