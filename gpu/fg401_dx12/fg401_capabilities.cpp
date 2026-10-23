// SPDX-License-Identifier: GPL-2.0-or-later
#include "fg401_support.h"
#define NOMINMAX
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>
static uint64_t Memory(VkPhysicalDevice p) {
    VkPhysicalDeviceMemoryProperties m{};
    vkGetPhysicalDeviceMemoryProperties(p, &m);
    uint64_t n{};
    for (uint32_t i = 0; i < m.memoryHeapCount; ++i)
        if (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            n = std::max(n, m.memoryHeaps[i].size);
    return n;
}
static bool Better(VkPhysicalDevice a, VkPhysicalDevice b) {
    VkPhysicalDeviceProperties x{}, y{};
    vkGetPhysicalDeviceProperties(a, &x);
    vkGetPhysicalDeviceProperties(b, &y);
    if ((x.apiVersion >= VK_API_VERSION_1_3) != (y.apiVersion >= VK_API_VERSION_1_3))
        return x.apiVersion >= VK_API_VERSION_1_3;
    if ((x.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) !=
        (y.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
        return x.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if ((x.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) !=
        (y.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU))
        return x.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
    return Memory(a) > Memory(b);
}
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    uint32_t version = Fg401Version;
    if (argc != 1) {
        if (argc != 3 || std::strcmp(argv[1], "--provider") ||
            (std::strcmp(argv[2], "4.0.1") && std::strcmp(argv[2], "4.0.2"))) return 2;
        if (!std::strcmp(argv[2], "4.0.2")) version = Fg402Version;
    }
    const auto result = [version]() -> Fg401Support {
        if (!Fg401WindowsSupported())
            return {false, "windows11"};
        VkApplicationInfo a{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        a.pApplicationName = "bbport FG support";
        a.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo c{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        c.pApplicationInfo = &a;
        struct Instance {
            VkInstance value{};
            ~Instance() {
                if (value)
                    vkDestroyInstance(value, nullptr);
            }
        } instance;
        const auto created = vkCreateInstance(&c, nullptr, &instance.value);
        if (created != VK_SUCCESS)
            return {false, created == VK_ERROR_INCOMPATIBLE_DRIVER ? "vulkan" : "check"};
        uint32_t count{};
        if (vkEnumeratePhysicalDevices(instance.value, &count, nullptr) != VK_SUCCESS || !count)
            return {false, "check"};
        std::vector<VkPhysicalDevice> devices(count);
        if (vkEnumeratePhysicalDevices(instance.value, &count, devices.data()) != VK_SUCCESS)
            return {false, "check"};
        auto selected = devices[0];
        if (const char *index = std::getenv("BB_GPU_ID")) {
            char *end{};
            errno = 0;
            auto i = std::strtol(index, &end, 10);
            if (end == index || *end || errno == ERANGE || (i >= 0 && static_cast<unsigned long>(i) >= count))
                return {false, "adapter"};
            if (i >= 0) selected = devices[i];
            else for (auto p : devices) if (Better(p, selected)) selected = p;
        } else
            for (auto p : devices)
                if (Better(p, selected))
                    selected = p;
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(selected, &props);
        if (!IsRx9000(props.properties.vendorID, props.properties.deviceName))
            return {false, "rx9000"};
        if (props.properties.apiVersion < VK_API_VERSION_1_3)
            return {false, "vulkan"};
        if (!id.deviceLUIDValid)
            return {false, "adapter"};
        uint32_t queues{};
        vkGetPhysicalDeviceQueueFamilyProperties(selected, &queues, nullptr);
        std::vector<VkQueueFamilyProperties> families(queues);
        vkGetPhysicalDeviceQueueFamilyProperties(selected, &queues, families.data());
        bool two = false;
        for (auto f : families)
            if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT)
                two = f.queueCount >= 2; // Match the renderer's last graphics queue family.
        if (!two)
            return {false, "interop"};
        uint32_t ne{};
        if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &ne, nullptr) != VK_SUCCESS)
            return {false, "check"};
        std::vector<VkExtensionProperties> extensions(ne);
        if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &ne, extensions.data()) != VK_SUCCESS)
            return {false, "check"};
        for (auto name : {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"})
            if (std::none_of(extensions.begin(), extensions.end(),
                             [&](auto &e) { return !std::strcmp(e.extensionName, name); }))
                return {false, "interop"};
        VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo external{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &timeline,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT};
        VkExternalSemaphoreProperties sharing{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(selected, &external, &sharing);
        if (!(sharing.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
            return {false, "interop"};
        return ProbeFg401(id.deviceLUID, version);
    }();
    std::printf("{\"supported\":%s,\"reason\":\"%s\",\"detail\":\"%s\",\"provider\":\"%s\"}\n",
                result.supported ? "true" : "false", result.reason.c_str(), result.detail.c_str(),
                Fg401VersionName(version));
    return 0;
}
