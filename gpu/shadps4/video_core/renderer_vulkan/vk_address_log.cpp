// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include "video_core/renderer_vulkan/vk_address_log.h"
#include "video_core/renderer_vulkan/vk_checkpoints.h"

namespace Vulkan::AddressLog {

namespace {

struct Range {
    u64 base{};
    u64 size{};
    u64 handle{};
    vk::ObjectType type{};
    bool internal{}; ///< the driver's own (VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT)
    u32 bound_frame{};
    s64 bound_ms{};
    u32 unbound_frame{};
    s64 unbound_ms{};
    char name[112]{}; ///< unbound ranges: their object's name when it was released
};

/// The last unbound ranges (the newest at num_unbound - 1).
constexpr u64 UnboundRingSize = 32768;

std::atomic<bool> on{false};
std::mutex mutex;
std::map<u64, Range> live; ///< by base address
Range unbound[UnboundRingSize];
u64 num_unbound = 0;
/// Names of images, views, buffers and allocations, by handle.
std::unordered_map<u64, std::string> names;
/// The image of each texture cache view, by view.
std::unordered_map<u64, u64> view_images;
u64 num_binds = 0;
/// The last fault address Dump was given (where Describe measures from).
u64 fault_address = 0;

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool Named(vk::ObjectType type) {
    return type == vk::ObjectType::eImage || type == vk::ObjectType::eBuffer ||
           type == vk::ObjectType::eDeviceMemory || type == vk::ObjectType::eImageView;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnReport(vk::DebugUtilsMessageSeverityFlagBitsEXT,
                                        vk::DebugUtilsMessageTypeFlagsEXT type,
                                        const vk::DebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding) || !data) {
        return VK_FALSE;
    }
    const vk::DeviceAddressBindingCallbackDataEXT* binding = nullptr;
    for (auto* next = static_cast<const vk::BaseInStructure*>(data->pNext); next;
         next = next->pNext) {
        if (next->sType == vk::StructureType::eDeviceAddressBindingCallbackDataEXT) {
            binding = reinterpret_cast<const vk::DeviceAddressBindingCallbackDataEXT*>(next);
            break;
        }
    }
    if (!binding) {
        return VK_FALSE;
    }
    Range range{
        .base = binding->baseAddress,
        .size = binding->size,
        .internal = static_cast<bool>(binding->flags &
                                      vk::DeviceAddressBindingFlagBitsEXT::eInternalObject),
        .bound_frame = Checkpoints::Frame(),
        .bound_ms = NowMs(),
    };
    if (data->objectCount > 0 && data->pObjects) {
        range.handle = data->pObjects[0].objectHandle;
        range.type = data->pObjects[0].objectType;
    }
    std::scoped_lock lock{mutex};
    if (binding->bindingType == vk::DeviceAddressBindingTypeEXT::eBind) {
        ++num_binds;
        names.erase(range.handle); // a new object: SetObjectName names it after the bind
        live[range.base] = range;
        return VK_FALSE;
    }
    // An unbind: the range bound at that address, with the name it had, goes to the ring.
    if (const auto it = live.find(range.base); it != live.end()) {
        const Range bound = it->second;
        range.bound_frame = bound.bound_frame;
        range.bound_ms = bound.bound_ms;
        range.size = std::max(range.size, bound.size);
        if (!range.handle) {
            range.handle = bound.handle;
            range.type = bound.type;
        }
        live.erase(it);
    } else {
        range.bound_frame = 0;
        range.bound_ms = 0;
    }
    range.unbound_frame = Checkpoints::Frame();
    range.unbound_ms = NowMs();
    if (const auto it = names.find(range.handle); it != names.end()) {
        std::snprintf(range.name, sizeof(range.name), "%s", it->second.c_str());
        names.erase(it);
    }
    unbound[num_unbound % UnboundRingSize] = range;
    ++num_unbound;
    return VK_FALSE;
}

void Print(const char* what, const Range& range, s64 now, u64 address) {
    const u64 end = range.base + range.size;
    char where[64];
    if (address < range.base) {
        std::snprintf(where, sizeof(where), "starts %#llx after it",
                      static_cast<unsigned long long>(range.base - address));
    } else if (address >= end) {
        std::snprintf(where, sizeof(where), "ends %#llx before it",
                      static_cast<unsigned long long>(address - end + 1));
    } else {
        std::snprintf(where, sizeof(where), "at +%#llx",
                      static_cast<unsigned long long>(address - range.base));
    }
    char when[96];
    if (range.unbound_ms) {
        std::snprintf(when, sizeof(when), "bound frame %u, released frame %u (%.1f s before)",
                      range.bound_frame, range.unbound_frame,
                      static_cast<double>(now - range.unbound_ms) / 1000.0);
    } else {
        std::snprintf(when, sizeof(when), "bound frame %u (%.1f s before)", range.bound_frame,
                      static_cast<double>(now - range.bound_ms) / 1000.0);
    }
    std::string name;
    if (range.unbound_ms) {
        name = range.name;
    } else if (const auto it = names.find(range.handle); it != names.end()) {
        name = it->second;
    }
    std::printf("GPU address %#llx: %s %s %#llx [%#llx, +%#llx) %s%s, %s: %s\n",
                static_cast<unsigned long long>(address), what,
                vk::to_string(range.type).c_str(), static_cast<unsigned long long>(range.handle),
                static_cast<unsigned long long>(range.base),
                static_cast<unsigned long long>(range.size), where,
                range.internal ? " (driver's own)" : "", when,
                name.empty() ? "(no name)" : name.c_str());
}

} // namespace

bool Wanted() {
    const char* value = std::getenv("BB_GPU_CHECKPOINTS");
    return value && (value[0] == '1' || value[0] == '2');
}

vk::UniqueDebugUtilsMessengerEXT Start(vk::Instance instance) {
    const vk::DebugUtilsMessengerCreateInfoEXT messenger_ci = {
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding,
        .pfnUserCallback = OnReport,
    };
    auto [result, messenger] = instance.createDebugUtilsMessengerEXTUnique(messenger_ci);
    if (result != vk::Result::eSuccess) {
        std::printf("GPU addresses: no messenger for the driver's reports (%s)\n",
                    vk::to_string(result).c_str());
        return {};
    }
    return std::move(messenger);
}

void Init(bool enabled) {
    if (!Wanted()) {
        return;
    }
    on.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        std::scoped_lock lock{mutex};
        std::printf("GPU addresses: on (VK_EXT_device_address_binding_report, %llu ranges bound "
                    "so far)\n",
                    static_cast<unsigned long long>(num_binds));
    } else {
        std::printf("GPU addresses: unavailable (no VK_EXT_device_address_binding_report)\n");
    }
}

void NoteName(vk::ObjectType type, u64 handle, std::string_view name) {
    if (!on.load(std::memory_order_relaxed) || !Named(type)) {
        return;
    }
    std::scoped_lock lock{mutex};
    names[handle] = name;
}

void NoteView(u64 view, u64 image) {
    if (!on.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{mutex};
    view_images[view] = image;
}

std::string DescribeView(u64 view) {
    std::string text;
    u64 image = 0;
    {
        std::scoped_lock lock{mutex};
        if (const auto it = names.find(view); it != names.end()) {
            text = it->second;
        }
        if (const auto it = view_images.find(view); it != view_images.end()) {
            image = it->second;
        }
    }
    if (text.empty()) {
        text = "(no name: not a texture cache view)";
    }
    if (image) {
        text += " -> " + Describe(image);
    }
    return text;
}

std::string Describe(u64 handle, u64 offset, u64 size) {
    if (!on.load(std::memory_order_relaxed) || !handle) {
        return "?";
    }
    std::scoped_lock lock{mutex};
    const Range* found = nullptr;
    for (const auto& [base, range] : live) {
        if (range.handle == handle && (!found || range.size > found->size)) {
            found = &range;
        }
    }
    bool released = false;
    if (!found) {
        const u64 oldest = num_unbound > UnboundRingSize ? num_unbound - UnboundRingSize : 0;
        for (u64 i = num_unbound; i-- > oldest;) {
            if (unbound[i % UnboundRingSize].handle == handle) {
                found = &unbound[i % UnboundRingSize];
                released = true;
                break;
            }
        }
    }
    char text[320];
    if (!found) {
        std::snprintf(text, sizeof(text), "%#llx: no GPU address known",
                      static_cast<unsigned long long>(handle));
        return text;
    }
    const u64 start = found->base + offset;
    const u64 end = size ? start + size : found->base + found->size;
    char where[80] = "";
    if (fault_address) {
        if (fault_address < start) {
            std::snprintf(where, sizeof(where), ", the fault %#llx before it",
                          static_cast<unsigned long long>(start - fault_address));
        } else if (fault_address >= end) {
            std::snprintf(where, sizeof(where), ", the fault %#llx past its end",
                          static_cast<unsigned long long>(fault_address - end + 1));
        } else {
            std::snprintf(where, sizeof(where), ", the fault inside it at +%#llx",
                          static_cast<unsigned long long>(fault_address - start));
        }
    }
    std::string name = released ? std::string{found->name} : std::string{};
    if (!released) {
        if (const auto it = names.find(handle); it != names.end()) {
            name = it->second;
        }
    }
    std::snprintf(text, sizeof(text), "%s %#llx [%#llx, +%#llx)%s%s: %s",
                  vk::to_string(found->type).c_str(), static_cast<unsigned long long>(handle),
                  static_cast<unsigned long long>(start),
                  static_cast<unsigned long long>(end - start), released ? " RELEASED" : "",
                  where, name.empty() ? "(no name)" : name.c_str());
    return text;
}

void Dump(u64 address, u64 precision) {
    if (!on.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{mutex};
    fault_address = address;
    const s64 now = NowMs();
    const u64 low = address > precision ? address - precision : 0;
    const u64 high = address + precision;
    // Bound now: everything over [low, high] (an allocation and the images in it overlap).
    const Range* below = nullptr;
    const Range* above = nullptr;
    u32 inside = 0;
    for (const auto& [base, range] : live) {
        const u64 end = range.base + range.size;
        if (range.base <= high && end > low) {
            Print("inside", range, now, address);
            ++inside;
        } else if (end <= low && (!below || end > below->base + below->size)) {
            below = &range;
        } else if (range.base > high && (!above || range.base < above->base)) {
            above = &range;
        }
    }
    if (inside == 0) {
        std::printf("GPU address %#llx: in nothing bound now (%zu ranges bound)\n",
                    static_cast<unsigned long long>(address), live.size());
        if (below) {
            Print("after", *below, now, address);
        }
        if (above) {
            Print("before", *above, now, address);
        }
    }
    // Released before, the newest first.
    u32 shown = 0;
    const u64 oldest = num_unbound > UnboundRingSize ? num_unbound - UnboundRingSize : 0;
    for (u64 i = num_unbound; i-- > oldest && shown < 8;) {
        const Range& range = unbound[i % UnboundRingSize];
        if (range.base <= high && range.base + range.size > low) {
            Print("released", range, now, address);
            ++shown;
        }
    }
    if (shown == 0) {
        std::printf("GPU address %#llx: in none of the last %llu ranges released\n",
                    static_cast<unsigned long long>(address),
                    static_cast<unsigned long long>(num_unbound - oldest));
    }
    std::fflush(stdout);
}

} // namespace Vulkan::AddressLog
