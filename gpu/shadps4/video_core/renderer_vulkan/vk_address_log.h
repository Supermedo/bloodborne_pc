// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

/// bbport: what a faulting GPU address belonged to (BB_GPU_CHECKPOINTS=1, beside the
/// checkpoints). The driver reports every GPU address range it binds or unbinds for an image,
/// buffer or memory allocation (VK_EXT_device_address_binding_report); this keeps the ranges bound
/// now and the last ones unbound, with the names SetObjectName gave their objects. After a device
/// loss, Dump says of a fault address (VK_EXT_device_fault) whether it lies in an object bound now,
/// in one released before and when, or between two objects: the ReadInvalid faults in the game's
/// post-processing passes (RTX 5090, 617.42) said nothing more than the address.
namespace Vulkan::AddressLog {

/// Whether BB_GPU_CHECKPOINTS asks for the crash report (the extension is enabled only then).
bool Wanted();

/// Before the device is made: the messenger the driver's reports arrive at.
vk::UniqueDebugUtilsMessengerEXT Start(vk::Instance instance);

/// After the device is made: whether the driver reports address bindings.
void Init(bool enabled);

/// SetObjectName: the name of an image, view, buffer or allocation, for the report.
void NoteName(vk::ObjectType type, u64 handle, std::string_view name);

/// ImageView: the image a texture cache view is of.
void NoteView(u64 view, u64 image);

/// For the checkpoints' bindings after a device loss: a view's name and the image it is of.
std::string DescribeView(u64 view);

/// The range of an image or buffer bound now (or released before), its name, and where the
/// fault address Dump was given lies from its part [offset, offset + size) (size 0: all of it).
std::string Describe(u64 handle, u64 offset = 0, u64 size = 0);

/// After a device loss: what is and was bound at `address` (+/- `precision`).
void Dump(u64 address, u64 precision);

} // namespace Vulkan::AddressLog
