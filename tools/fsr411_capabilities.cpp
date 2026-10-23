// SPDX-License-Identifier: GPL-2.0-or-later
#include "fsr411_support.h"
#include "fsr411_vulkan_support.h"
#include "driver_provider.h"
#include "ffx_upscale.h"
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <windows.h>

namespace {
void String(const std::string& value) {
    std::putchar('"');
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') std::printf("\\%c", c);
        else if (c < 32) std::printf("\\u%04x", c);
        else std::putchar(c);
    }
    std::putchar('"');
}
void Status(const Fsr411Support& status, const FsrSupportTrace& trace, bool close = true) {
    std::printf("{\"supported\":%s,\"reason\":", status.supported ? "true" : "false");
    String(status.reason);
    std::printf(",\"detail\":"); String(status.detail);
    std::printf(",\"checks\":[");
    bool first = true;
    for (const auto& check : trace) {
        if (!first) std::putchar(',');
        first = false;
        std::printf("{\"id\":"); String(check.id);
        std::printf(",\"state\":"); String(check.state);
        std::printf(",\"detail\":"); String(check.detail);
        std::putchar('}');
    }
    std::putchar(']');
    if (close) std::putchar('}');
}
void Gpu(VkPhysicalDevice physical) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &props);
    const auto& p = props.properties;
    std::printf("{\"name\":"); String(p.deviceName);
    std::printf(",\"vendor_id\":%u,\"device_id\":%u,\"driver_name\":", p.vendorID, p.deviceID);
    String(driver.driverName);
    std::printf(",\"driver_info\":"); String(driver.driverInfo);
    std::printf(",\"vulkan\":\"%u.%u.%u\"}", VK_API_VERSION_MAJOR(p.apiVersion),
        VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
}
void Result(VkPhysicalDevice physical, const Fsr411Support& status, const FsrSupportTrace& trace) {
    Status(status, trace, false);
    std::printf(",\"gpu\":"); Gpu(physical);
    std::printf("}\n");
}
}

extern "C" void PrintFsr412Capabilities(VkPhysicalDevice physical, int dx12) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    FsrSupportTrace trace;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    const bool amd = properties.vendorID == 0x1002;
    FsrCheck(&trace, "gpu", amd ? "pass" : "fail", "AMD");
    if (!amd) {
        Result(physical, {false, "hardware", "AMD GPU required"}, trace);
        return;
    }
    if (dx12) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        const auto status = id.deviceLUIDValid ? ProbeFsr412Dx12(id.deviceLUID, &trace)
            : Fsr411Support{false, "adapter", "Vulkan/DX12 adapter LUID"};
        if (!id.deviceLUIDValid) FsrCheck(&trace, "adapter", "fail", status.detail);
        Result(physical, status, trace);
        return;
    }
    VkDevice device{};
    Fsr411Support status{false, "driver", "AMD FSR 4.1.2 Vulkan provider unavailable"};
    const char* stage = "queue";
    try {
        uint32_t count{};
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == count) throw std::runtime_error("Vulkan graphics queue unavailable");
        FsrCheck(&trace, stage, "pass", "family=" + std::to_string(family));
        stage = "device";
        const float priority = 1.f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1; create.pQueueCreateInfos = &queue;
        const auto result = vkCreateDevice(physical, &create, nullptr, &device);
        if (result != VK_SUCCESS)
            throw std::runtime_error("vkCreateDevice=" + std::to_string(result));
        FsrCheck(&trace, stage, "pass", "Vulkan preflight device");
        stage = "runtime_load";
        const auto module = DriverFsr::Runtime(false);
        FsrCheck(&trace, stage, "pass", "amdvkffx64.dll");
        stage = "provider_query";
        const auto provider = DriverFsr::Get(module, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
        char identity[64]{};
        std::snprintf(identity, sizeof(identity), "0x%llx", (unsigned long long)provider.provider.versionId);
        FsrCheck(&trace, stage, "pass", std::string(identity) + " " +
            (provider.provider.versionName ? provider.provider.versionName : ""));
        stage = "provider_version";
        const bool version = (uint32_t(provider.provider.versionId) & 0x7fffffffu) ==
                             ((4u << 22) | (1u << 12) | 2u);
        FsrCheck(&trace, stage, version ? "pass" : "fail", "4.1.2");
        if (!version) {
            status = {false, "driver", "FSR 4.1.2 provider unavailable"};
        } else {
            stage = "provider_support";
            const struct { VkDevice device; VkPhysicalDevice physical; PFN_vkGetDeviceProcAddr gdpa; }
                backend{device, physical, &vkGetDeviceProcAddr};
            const bool supported = DriverFsr::Supported(provider, &backend.device);
            FsrCheck(&trace, stage, supported ? "pass" : "fail",
                supported ? "isSupported=true" : "isSupported=false");
            status = {supported, supported ? "ready" : "hardware",
                supported ? "4.1.2" : "FSR 4.1.2 unsupported by selected GPU"};
        }
    } catch (const std::exception& error) {
        FsrCheck(&trace, stage, "fail", error.what());
        status = {false, (std::string_view(stage) == "queue" || std::string_view(stage) == "device" ||
                         std::string_view(stage) == "provider_support")
            ? "check" : "driver", error.what()};
    }
    if (device) vkDestroyDevice(device, nullptr);
    Result(physical, status, trace);
}

extern "C" void PrintFsr411Capabilities(VkPhysicalDevice device) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(device, &props);
    FsrSupportTrace shared, dx, vk;
    const auto interop = ProbeFsrDx12Interop(device, &shared);
    const auto dx12 = interop.supported ? ProbeFsr411Dx12(id.deviceLUID, &dx) : interop;
    const char* missing = Fsr411VulkanRequirement(device, vkGetPhysicalDeviceProperties,
        vkEnumerateDeviceExtensionProperties, vkGetPhysicalDeviceFeatures2);
    FsrCheck(&vk, "vulkan_features", missing ? "fail" : "pass", missing ? missing :
        "Vulkan 1.3; INT8/FP16; integer dot product; compute derivatives; mutable descriptors; descriptor buffers");
    const Fsr411Support vulkan{!missing,
        !missing ? "ready" : !std::strcmp(missing, "Vulkan extension query") ? "check" : "hardware",
        missing ? missing : ""};
    std::printf("{\"vendor_id\":%u,\"dx12\":", props.properties.vendorID); Status(dx12, dx);
    std::printf(",\"vk\":"); Status(vulkan, vk);
    std::printf(",\"dx12_interop\":"); Status(interop, shared);
    std::printf(",\"gpu\":"); Gpu(device);
    std::printf("}\n");
}
