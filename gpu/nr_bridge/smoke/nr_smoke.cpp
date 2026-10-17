// SPDX-FileCopyrightText: Copyright 2026 bbport contributors, bbhost contributors
// SPDX-License-Identifier: MIT
//
// A headless Vulkan program that runs the Neural Rendering bridge over a synthetic frame and checks
// that the model actually changed pixels.
//
// This is the smallest thing that can prove the bridge works: no window, no swapchain, no game. It
// creates a device the way the port does (the model's four extensions plus bufferDeviceAddress),
// hands the model a colour/motion/depth triplet and an output, and compares what comes back against
// what went in. A bridge that returns success but writes the input through unchanged is exactly the
// failure this exists to catch, so the verdict is on the pixels, not on the return codes.
//
// Vulkan is resolved from vulkan-1.dll at run time, so this needs the headers and nothing else.
//
//   nr_smoke.exe [--runtime <dir with nvngx_dlssnr.dll>] [--bridge <dir with bbport_nr_nvngx.dll>]
//                [--size <n>] [--frames <n>] [--preset <0..2>] [--style <0..2>] [--intensity <0..1>]
//                [--structure <0..1>] [--tone <0..1>] [--auto-skin <0|1>]
//                [--skin-structure <-1..1>] [--input <raw RGBA16F>] [--allow-identity]
//
// The tuning flags exist so the controls the launcher and the F10 menu expose can be exercised
// headlessly, one value at a time, instead of only at their defaults. The defaults are the runtime's
// own: style 0 (Default), full strength, no automatic skin mask.
//
// --input replaces the synthetic pattern with a real frame: a raw RGBA16F buffer of exactly
// size*size*8 bytes, which is the format the model's colour input already uses, so a crop of an
// actual game frame goes in with no conversion and no guessing about the layout. The synthetic
// pattern is a fine smoke test for "the model ran", but it cannot show what a style or an intensity
// does to a face or a texture, and that is what the tuning flags are for.
//
// --intensity 0 and --allow-identity both accept an unchanged picture. Zero means "leave the picture
// alone", so a correct model returns the input through, and the pixel-change bar below would fail a
// working pass for doing exactly what it was asked; --allow-identity says the same for any other
// tuning, for the runs that are checking a parameter is accepted rather than that it does something.
// In both cases the output still has to be written and finite.
//
// Exit code 0 means the model ran and the pixels changed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bbport_nr_bridge.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Vulkan entry points, resolved at run time
// ---------------------------------------------------------------------------------------------

struct Vk {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;

    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions = nullptr;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = nullptr;
};

Vk vk;

void Log(int warning, const char* message) {
    std::printf("%s %s\n", warning ? "[warn ]" : "[info ]", message);
    std::fflush(stdout);
}

void Fail(const char* message) {
    std::printf("[fail ] %s\n", message);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

struct Setup {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& entry : list) {
        if (std::strcmp(entry.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool LoadVulkan() {
    HMODULE module = LoadLibraryW(L"vulkan-1.dll");

    if (module == nullptr) {
        Fail("vulkan-1.dll is not installed");
        return false;
    }

    vk.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(module, "vkGetInstanceProcAddr"));

    if (vk.get_instance_proc == nullptr) {
        Fail("vulkan-1.dll has no vkGetInstanceProcAddr");
        return false;
    }

    vk.create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(vk.get_instance_proc(nullptr, "vkCreateInstance"));

    if (vk.create_instance == nullptr) {
        Fail("no vkCreateInstance");
        return false;
    }

    return true;
}

bool CreateInstance(Setup& setup, bool& have_properties2) {
    std::vector<VkExtensionProperties> available;
    uint32_t count = 0;
    vk.enumerate_instance_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vk.get_instance_proc(nullptr, "vkEnumerateInstanceExtensionProperties"));

    if (vk.enumerate_instance_extensions != nullptr &&
        vk.enumerate_instance_extensions(nullptr, &count, nullptr) == VK_SUCCESS && count > 0) {
        available.resize(count);
        vk.enumerate_instance_extensions(nullptr, &count, available.data());
    }

    // Core from 1.1, so this is belt and braces: the model names it, and a 1.0 instance would need it.
    have_properties2 = HasExtension(available, "VK_KHR_get_physical_device_properties2");

    const char* extensions[] = {"VK_KHR_get_physical_device_properties2"};

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bbport nr smoke";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;

    if (have_properties2) {
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
    }

    if (vk.create_instance(&info, nullptr, &setup.instance) != VK_SUCCESS) {
        Fail("vkCreateInstance failed");
        return false;
    }

    return true;
}

bool LoadInstanceFns(VkInstance instance) {
    const auto load = [instance](const char* name) -> PFN_vkVoidFunction {
        return vk.get_instance_proc(instance, name);
    };

    vk.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(load("vkDestroyInstance"));
    vk.enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(load("vkEnumeratePhysicalDevices"));
    vk.get_physical_device_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        load("vkGetPhysicalDeviceProperties"));
    vk.get_physical_device_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        load("vkGetPhysicalDeviceFeatures2"));
    vk.get_physical_device_memory_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            load("vkGetPhysicalDeviceMemoryProperties"));
    vk.get_queue_family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        load("vkGetPhysicalDeviceQueueFamilyProperties"));
    vk.enumerate_device_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        load("vkEnumerateDeviceExtensionProperties"));
    vk.create_device = reinterpret_cast<PFN_vkCreateDevice>(load("vkCreateDevice"));
    vk.get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(load("vkGetDeviceProcAddr"));

    return vk.enumerate_physical_devices != nullptr && vk.get_physical_device_properties != nullptr &&
           vk.get_queue_family_properties != nullptr && vk.create_device != nullptr &&
           vk.get_device_proc != nullptr;
}

bool PickDevice(Setup& setup, const BbNrApi* api, std::vector<const char*>& device_extensions) {
    uint32_t count = 0;

    if (vk.enumerate_physical_devices(setup.instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        Fail("no Vulkan physical devices");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vk.enumerate_physical_devices(setup.instance, &count, devices.data());

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vk.get_physical_device_properties(candidate, &properties);

        if (properties.vendorID != 0x10DE) {
            continue;
        }

        std::printf("[info ] device: %s (driver 0x%08x)\n", properties.deviceName,
                    properties.driverVersion);

        std::vector<VkExtensionProperties> available;
        uint32_t extension_count = 0;
        vk.enumerate_device_extensions(candidate, nullptr, &extension_count, nullptr);
        available.resize(extension_count);
        vk.enumerate_device_extensions(candidate, nullptr, &extension_count, available.data());

        uint32_t required_count = 0;
        const VkExtensionProperties* required = nullptr;

        if (api->DeviceExtensions(setup.instance, candidate, &required_count, &required) != 1) {
            Fail("the bridge would not report its device extensions");
            return false;
        }

        bool complete = true;

        for (uint32_t i = 0; i < required_count; ++i) {
            const bool present = HasExtension(available, required[i].extensionName);
            std::printf("[info ] %s %s\n", present ? "have   " : "MISSING",
                        required[i].extensionName);
            complete &= present;

            if (present) {
                device_extensions.push_back(required[i].extensionName);
            }
        }

        if (!complete) {
            Fail("this device does not offer every extension the model needs");
            return false;
        }

        setup.physical = candidate;
        return true;
    }

    Fail("no NVIDIA physical device found; Neural Rendering is an RTX feature");
    return false;
}

bool CreateDevice(Setup& setup, const std::vector<const char*>& device_extensions) {
    // vkGetDeviceQueue is a device-level entry point and is resolved later, so it is not checked here.
    if (vk.get_physical_device_features2 == nullptr || vk.create_device == nullptr) {
        Fail("an instance-level entry point the device setup needs is missing");
        return false;
    }

    uint32_t family_count = 0;
    vk.get_queue_family_properties(setup.physical, &family_count, nullptr);
    std::printf("[info ] queue families: %u\n", family_count);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vk.get_queue_family_properties(setup.physical, &family_count, families.data());

    bool found = false;

    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            setup.queue_family = i;
            found = true;
            break;
        }
    }

    if (!found) {
        Fail("no compute queue family");
        return false;
    }

    // The model's kernels are loaded through buffer device addresses, so the feature has to be on,
    // not merely supported.
    VkPhysicalDeviceBufferDeviceAddressFeatures address{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    address.bufferDeviceAddress = VK_TRUE;

    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &address;
    vk.get_physical_device_features2(setup.physical, &supported);
    std::printf("[info ] bufferDeviceAddress supported: %d\n",
                address.bufferDeviceAddress == VK_TRUE ? 1 : 0);

    if (address.bufferDeviceAddress != VK_TRUE) {
        Fail("bufferDeviceAddress is not available on this device");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = setup.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &address;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    info.ppEnabledExtensionNames = device_extensions.data();

    if (vk.create_device(setup.physical, &info, nullptr, &setup.device) != VK_SUCCESS) {
        Fail("vkCreateDevice failed");
        return false;
    }
    std::printf("[info ] device created with %zu extensions\n", device_extensions.size());

    // Resolved here rather than in LoadDeviceFns: this is the first thing that needs it, and calling
    // through a not-yet-resolved pointer is a null call.
    vk.get_device_queue =
        reinterpret_cast<PFN_vkGetDeviceQueue>(vk.get_device_proc(setup.device, "vkGetDeviceQueue"));

    if (vk.get_device_queue == nullptr) {
        Fail("vkGetDeviceQueue could not be resolved");
        return false;
    }

    vk.get_device_queue(setup.device, setup.queue_family, 0, &setup.queue);
    return true;
}

bool LoadDeviceFns(VkDevice device) {
    if (vk.get_device_proc == nullptr) {
        Fail("vkGetDeviceProcAddr was not resolved");
        return false;
    }

    const auto load = [device](const char* name) -> PFN_vkVoidFunction {
        return vk.get_device_proc(device, name);
    };

    vk.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(load("vkDestroyDevice"));
    vk.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(load("vkDeviceWaitIdle"));
    vk.create_command_pool =
        reinterpret_cast<PFN_vkCreateCommandPool>(load("vkCreateCommandPool"));
    vk.destroy_command_pool =
        reinterpret_cast<PFN_vkDestroyCommandPool>(load("vkDestroyCommandPool"));
    vk.allocate_command_buffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(load("vkAllocateCommandBuffers"));
    vk.begin_command_buffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(load("vkBeginCommandBuffer"));
    vk.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(load("vkEndCommandBuffer"));
    vk.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(load("vkQueueSubmit"));
    vk.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(load("vkQueueWaitIdle"));
    vk.create_image = reinterpret_cast<PFN_vkCreateImage>(load("vkCreateImage"));
    vk.destroy_image = reinterpret_cast<PFN_vkDestroyImage>(load("vkDestroyImage"));
    vk.get_image_memory_requirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(load("vkGetImageMemoryRequirements"));
    vk.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(load("vkAllocateMemory"));
    vk.free_memory = reinterpret_cast<PFN_vkFreeMemory>(load("vkFreeMemory"));
    vk.bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(load("vkBindImageMemory"));
    vk.create_image_view = reinterpret_cast<PFN_vkCreateImageView>(load("vkCreateImageView"));
    vk.destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(load("vkDestroyImageView"));
    vk.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(load("vkCreateBuffer"));
    vk.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(load("vkDestroyBuffer"));
    vk.get_buffer_memory_requirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(load("vkGetBufferMemoryRequirements"));
    vk.bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(load("vkBindBufferMemory"));
    vk.map_memory = reinterpret_cast<PFN_vkMapMemory>(load("vkMapMemory"));
    vk.unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(load("vkUnmapMemory"));
    vk.cmd_pipeline_barrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(load("vkCmdPipelineBarrier"));
    vk.cmd_copy_buffer_to_image =
        reinterpret_cast<PFN_vkCmdCopyBufferToImage>(load("vkCmdCopyBufferToImage"));
    vk.cmd_copy_image_to_buffer =
        reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(load("vkCmdCopyImageToBuffer"));

    return vk.create_command_pool != nullptr && vk.allocate_command_buffers != nullptr &&
           vk.queue_submit != nullptr && vk.create_image != nullptr &&
           vk.allocate_memory != nullptr && vk.create_image_view != nullptr &&
           vk.create_buffer != nullptr && vk.cmd_pipeline_barrier != nullptr &&
           vk.cmd_copy_buffer_to_image != nullptr && vk.cmd_copy_image_to_buffer != nullptr;
}

uint32_t FindMemoryType(const Setup& setup, uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties properties{};
    vk.get_physical_device_memory_properties(setup.physical, &properties);

    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }

    return UINT32_MAX;
}

bool CreateImage(const Setup& setup, Image& out, uint32_t width, uint32_t height, VkFormat format) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vk.create_image(setup.device, &info, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vk.get_image_memory_requirements(setup.device, out.image, &requirements);

    const uint32_t type = FindMemoryType(setup, requirements.memoryTypeBits, 0);

    if (type == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    if (vk.allocate_memory(setup.device, &allocate, nullptr, &out.memory) != VK_SUCCESS) {
        return false;
    }
    if (vk.bind_image_memory(setup.device, out.image, out.memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vk.create_image_view(setup.device, &view, nullptr, &out.view) != VK_SUCCESS) {
        return false;
    }

    out.format = format;
    out.width = width;
    out.height = height;
    return true;
}

void DestroyImage(const Setup& setup, Image& image) {
    if (image.view != VK_NULL_HANDLE) {
        vk.destroy_image_view(setup.device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vk.destroy_image(setup.device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vk.free_memory(setup.device, image.memory, nullptr);
    }
    image = Image{};
}

bool CreateHostBuffer(const Setup& setup, VkDeviceSize size, VkBuffer& buffer,
                      VkDeviceMemory& memory, void** mapped) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vk.create_buffer(setup.device, &info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vk.get_buffer_memory_requirements(setup.device, buffer, &requirements);

    const uint32_t type = FindMemoryType(setup, requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (type == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    if (vk.allocate_memory(setup.device, &allocate, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    if (vk.bind_buffer_memory(setup.device, buffer, memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (vk.map_memory(setup.device, memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) {
        return false;
    }

    return true;
}

VkCommandBuffer BeginCommands(const Setup& setup) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = setup.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;

    VkCommandBuffer command = VK_NULL_HANDLE;
    vk.allocate_command_buffers(setup.device, &allocate, &command);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.begin_command_buffer(command, &begin);

    return command;
}

bool Submit(const Setup& setup, VkCommandBuffer command) {
    vk.end_command_buffer(command);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;

    if (vk.queue_submit(setup.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }

    return vk.queue_wait_idle(setup.queue) == VK_SUCCESS;
}

void Transition(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;

    vk.cmd_pipeline_barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);
}

// ---------------------------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------------------------

uint16_t FloatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }

    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 ((mantissa + 0x1000u) >> 13));
}

float HalfToFloat(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x3FFu;
    uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool IsNonFinite(uint16_t value) {
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    return exponent == 0x1Fu && mantissa != 0;
}

uint8_t ToByte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool WritePpm(const char* path, const uint16_t* rgba, uint32_t width, uint32_t height) {
    FILE* file = std::fopen(path, "wb");

    if (file == nullptr) {
        return false;
    }

    std::fprintf(file, "P6\n%u %u\n255\n", width, height);

    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        const uint8_t pixel[3] = {ToByte(HalfToFloat(rgba[i * 4 + 0])),
                                  ToByte(HalfToFloat(rgba[i * 4 + 1])),
                                  ToByte(HalfToFloat(rgba[i * 4 + 2]))};
        std::fwrite(pixel, 1, sizeof(pixel), file);
    }

    std::fclose(file);
    return true;
}

bool WriteRaw(const char* path, const void* data, size_t bytes) {
    FILE* file = std::fopen(path, "wb");

    if (file == nullptr) {
        return false;
    }

    std::fwrite(data, 1, bytes, file);
    std::fclose(file);
    return true;
}

// A real frame instead of the synthetic pattern: raw RGBA16F, which is the format the model's colour
// input already uses. The size is checked rather than trusted -- a buffer of the wrong length would
// otherwise be read past its end, and a silently short read would show up as a model that "did
// nothing" rather than as the bad file it is.
bool ReadColourFile(const std::wstring& path, std::vector<uint16_t>& out, uint32_t width,
                    uint32_t height) {
    const size_t expected = static_cast<size_t>(width) * height * 8;
    FILE* file = _wfopen(path.c_str(), L"rb");

    if (file == nullptr) {
        std::printf("[fail ] --input: %ls could not be opened\n", path.c_str());
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long bytes = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (bytes < 0 || static_cast<size_t>(bytes) != expected) {
        std::printf("[fail ] --input: %ls is %ld bytes, but %ux%u RGBA16F is %zu\n", path.c_str(),
                    bytes, width, height, expected);
        std::fclose(file);
        return false;
    }

    out.resize(expected / sizeof(uint16_t));

    if (std::fread(out.data(), 1, expected, file) != expected) {
        std::printf("[fail ] --input: %ls ended before its %zu bytes were read\n", path.c_str(),
                    expected);
        std::fclose(file);
        return false;
    }

    std::fclose(file);
    return true;
}

// A synthetic frame with structure at several scales: a gradient, a checkerboard and a sine weave.
// Flat colour would let a broken model look like a working one.
void FillColour(std::vector<float>& pixels, uint32_t width, uint32_t height) {
    pixels.assign(static_cast<size_t>(width) * height * 4, 1.0f);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width - 1);
            const float v = static_cast<float>(y) / static_cast<float>(height - 1);
            const float base = 0.20f + 0.45f * u;
            const float checker = (((x / 16) + (y / 16)) % 2 == 0) ? 0.14f : -0.14f;
            const float weave = 0.09f * std::sin(u * 37.0f) * std::cos(v * 29.0f);

            const size_t at = (static_cast<size_t>(y) * width + x) * 4;
            pixels[at + 0] = std::clamp(base + checker + weave, 0.0f, 1.0f);
            pixels[at + 1] = std::clamp(base * 0.85f + weave, 0.0f, 1.0f);
            pixels[at + 2] = std::clamp(0.55f * base + checker * 0.5f, 0.0f, 1.0f);
            pixels[at + 3] = 1.0f;
        }
    }
}

std::wstring ExeDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring text = path.data();
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

std::wstring Widen(const char* utf8) {
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), needed);
    wide.resize(std::wcslen(wide.c_str()));
    return wide;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: if the runtime takes the process down, the last line printed says where.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::wstring runtime_directory = ExeDirectory();
    std::wstring bridge_directory = ExeDirectory();
    uint32_t size = 256;
    int frames = 4;

    // The model's tuning, at the runtime's defaults. These are read once at CreatePass, so a smoke
    // run covers exactly one point in the control space; the ranges match BbNeural::Pass, which is
    // what the launcher and the in-game menu write.
    uint32_t preset = 0;
    uint32_t style = 0;
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 1.0f;
    uint32_t auto_skin = 0;
    float skin_structure = 1.0f;
    std::wstring input_path;
    bool allow_identity = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--runtime" && i + 1 < argc) {
            runtime_directory = Widen(argv[++i]);
        } else if (arg == "--bridge" && i + 1 < argc) {
            bridge_directory = Widen(argv[++i]);
        } else if (arg == "--size" && i + 1 < argc) {
            size = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--preset" && i + 1 < argc) {
            preset = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--style" && i + 1 < argc) {
            style = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--intensity" && i + 1 < argc) {
            intensity = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--structure" && i + 1 < argc) {
            structure = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--tone" && i + 1 < argc) {
            tone = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--auto-skin" && i + 1 < argc) {
            auto_skin = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--skin-structure" && i + 1 < argc) {
            skin_structure = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--input" && i + 1 < argc) {
            input_path = Widen(argv[++i]);
        } else if (arg == "--allow-identity") {
            allow_identity = true;
        } else {
            // Refused rather than ignored: a flag that was mistyped, or that this build does not
            // know, would otherwise leave the test running at a default while its verdict is read as
            // if the flag had applied.
            std::printf("[fail ] unrecognised or incomplete argument: %s\n", arg.c_str());
            std::fflush(stdout);
            return 2;
        }
    }

    // Refused rather than clamped: a run that silently tests a different value than it was asked to
    // is worse than one that stops, because its verdict is then attributed to the wrong setting.
    if (preset > 2) {
        Fail("--preset takes 0 (Automatic), 1 (Original) or 2 (Optimized)");
        return 2;
    }
    if (style > 2) {
        Fail("--style takes 0, 1 or 2 (the model's DLSSNR.Style)");
        return 2;
    }
    if (intensity < 0.0f || intensity > 1.0f || structure < 0.0f || structure > 1.0f ||
        tone < 0.0f || tone > 1.0f || skin_structure < -1.0f || skin_structure > 1.0f) {
        Fail("--intensity, --structure and --tone take 0..1, --skin-structure takes -1..1");
        return 2;
    }
    if (auto_skin > 1) {
        Fail("--auto-skin takes 0 or 1");
        return 2;
    }
    if (size < 2) {
        Fail("--size must be at least 2: the synthetic frame and the depth ramp divide by width-1");
        return 2;
    }

    std::printf("[info ] Neural Rendering smoke test, %ux%u, %d evaluations\n", size, size, frames);
    std::printf("[info ] preset %u, style %u, intensity %.3f, structure %.3f, tone %.3f, auto-skin %u, "
                "skin-structure %.3f\n",
                preset, style, static_cast<double>(intensity), static_cast<double>(structure),
                static_cast<double>(tone), auto_skin, static_cast<double>(skin_structure));
    std::fflush(stdout);

    // The colour frame is settled before any Vulkan object exists: a missing or mis-sized --input is
    // a file problem, and reporting it should not cost a device.
    std::vector<uint16_t> colour_half;

    if (input_path.empty()) {
        std::vector<float> colour_pixels;
        FillColour(colour_pixels, size, size);

        colour_half.resize(colour_pixels.size());
        for (size_t i = 0; i < colour_pixels.size(); ++i) {
            colour_half[i] = FloatToHalf(colour_pixels[i]);
        }
    } else if (!ReadColourFile(input_path, colour_half, size, size)) {
        return 2;
    } else {
        std::printf("[info ] colour input read from %ls\n", input_path.c_str());
    }

    // The bridge first: it is what everything else is for, and it is cheap to refuse.
    std::wstring bridge_path = bridge_directory + L"\\bbport_nr_nvngx.dll";
    HMODULE bridge = LoadLibraryExW(bridge_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (bridge == nullptr) {
        bridge_path = bridge_directory + L"\\bbport_nr.dll";
        bridge = LoadLibraryExW(bridge_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (bridge == nullptr) {
        Fail("bbport_nr_nvngx.dll was not found beside the smoke test; pass --bridge <dir>");
        return 2;
    }

    auto get_api = reinterpret_cast<BbNrGetApiFn>(GetProcAddress(bridge, "BbNrGetApi"));

    if (get_api == nullptr) {
        Fail("the bridge has no BbNrGetApi export");
        return 2;
    }

    const BbNrApi* api = get_api();

    if (api == nullptr || api->abi != BBPORT_NR_BRIDGE_ABI || api->size != sizeof(BbNrApi)) {
        Fail("the bridge reports an ABI this smoke test does not understand");
        return 2;
    }

    std::printf("[info ] bridge ABI %u, size %u, max passes %u\n", api->abi, api->size,
                BBPORT_NR_MAX_PASSES);

    if (!LoadVulkan()) {
        return 2;
    }

    Setup setup;
    bool have_properties2 = false;

    if (!CreateInstance(setup, have_properties2) || !LoadInstanceFns(setup.instance)) {
        return 2;
    }

    std::printf("[info ] VK_KHR_get_physical_device_properties2 %s\n",
                have_properties2 ? "enabled" : "not offered");

    if (api->Configure(runtime_directory.c_str(), runtime_directory.c_str(), Log) != 1) {
        Fail("Configure failed");
        return 2;
    }

    uint32_t instance_extension_count = 0;
    const VkExtensionProperties* instance_extensions = nullptr;
    api->InstanceExtensions(&instance_extension_count, &instance_extensions);

    for (uint32_t i = 0; i < instance_extension_count; ++i) {
        std::printf("[info ] instance extension requested: %s\n",
                    instance_extensions[i].extensionName);
    }

    std::vector<const char*> device_extensions;

    if (!PickDevice(setup, api, device_extensions) || !CreateDevice(setup, device_extensions) ||
        !LoadDeviceFns(setup.device)) {
        return 2;
    }

    if (api->Initialize(setup.instance, setup.physical, setup.device, vk.get_instance_proc,
                        vk.get_device_proc) != 1) {
        Fail("Initialize failed; see the lines above");
        return 3;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = setup.queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk.create_command_pool(setup.device, &pool_info, nullptr, &setup.pool);

    Image color;
    Image motion;
    Image depth;
    Image output;

    const bool images_ok = CreateImage(setup, color, size, size, VK_FORMAT_R16G16B16A16_SFLOAT) &&
                           CreateImage(setup, motion, size, size, VK_FORMAT_R16G16_SFLOAT) &&
                           CreateImage(setup, depth, size, size, VK_FORMAT_R32_SFLOAT) &&
                           CreateImage(setup, output, size, size, VK_FORMAT_R16G16B16A16_SFLOAT);

    if (!images_ok) {
        Fail("the smoke test could not allocate its images");
        return 2;
    }

    std::vector<uint16_t> motion_half(static_cast<size_t>(size) * size * 2, 0);
    std::vector<float> depth_float(static_cast<size_t>(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            depth_float[static_cast<size_t>(y) * size + x] =
                0.30f + 0.40f * (static_cast<float>(x) / static_cast<float>(size - 1));
        }
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;

    const VkDeviceSize colour_bytes = colour_half.size() * sizeof(uint16_t);
    const VkDeviceSize motion_bytes = motion_half.size() * sizeof(uint16_t);
    const VkDeviceSize depth_bytes = depth_float.size() * sizeof(float);
    const VkDeviceSize read_bytes = static_cast<VkDeviceSize>(size) * size * 8;

    // One staging buffer, four regions at distinct aligned offsets. The GPU reads these when the
    // copies execute, not when they are recorded, so a shared offset would hand every image whatever
    // was written last -- which is exactly how a smoke test ends up comparing depth against depth and
    // calling it a pass.
    const auto align_up = [](VkDeviceSize value) { return (value + 15) & ~VkDeviceSize(15); };

    const VkDeviceSize colour_offset = 0;
    const VkDeviceSize motion_offset = align_up(colour_offset + colour_bytes);
    const VkDeviceSize depth_offset = align_up(motion_offset + motion_bytes);
    const VkDeviceSize read_offset = align_up(depth_offset + depth_bytes);
    const VkDeviceSize staging_bytes = read_offset + read_bytes;

    if (!CreateHostBuffer(setup, staging_bytes, staging, staging_memory, &mapped)) {
        Fail("the smoke test could not allocate its staging buffer");
        return 2;
    }

    // Upload the three inputs and put the output in GENERAL, which is where the model writes.
    {
        VkCommandBuffer command = BeginCommands(setup);

        const auto upload = [&](Image& image, const void* data, VkDeviceSize bytes,
                                VkDeviceSize offset) {
            std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, static_cast<size_t>(bytes));

            Transition(command, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {image.width, image.height, 1};
            vk.cmd_copy_buffer_to_image(command, staging, image.image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            Transition(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT);
        };

        upload(color, colour_half.data(), colour_bytes, colour_offset);
        upload(motion, motion_half.data(), motion_bytes, motion_offset);
        upload(depth, depth_float.data(), depth_bytes, depth_offset);

        Transition(command, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                   VK_ACCESS_SHADER_WRITE_BIT);

        if (!Submit(setup, command)) {
            Fail("the upload submission failed");
            return 2;
        }
    }

    BbNrTuning tuning{};
    tuning.width = size;
    tuning.height = size;
    tuning.preset = preset;
    tuning.style = style;
    tuning.intensity = intensity;
    tuning.structure = structure;
    tuning.tone = tone;
    tuning.skin_structure = skin_structure;
    tuning.auto_skin = auto_skin;
    tuning.ui_correction = 0;

    // Creation records its uploads into this buffer, so it is submitted on its own and the pass is
    // polled ready afterwards. Evaluating in the same submission is the bug this guards against.
    {
        VkCommandBuffer command = BeginCommands(setup);

        if (api->CreatePass(command, 0, &tuning) != 1) {
            Fail("CreatePass failed");
            return 3;
        }
        if (!Submit(setup, command)) {
            Fail("the creation submission failed");
            return 3;
        }
    }

    if (api->ReadyPass(0) != 1) {
        Fail("the pass did not report ready after its creation submission retired");
        return 3;
    }

    BbNrEvaluate evaluate{};
    evaluate.color = {color.image, color.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, color.format,
                      color.width, color.height};
    evaluate.motion = {motion.image, motion.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                       motion.format, motion.width, motion.height};
    evaluate.depth = {depth.image, depth.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, depth.format,
                      depth.width, depth.height};
    evaluate.output = {output.image, output.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                       output.format, output.width, output.height};
    evaluate.depth_inverted = 0;
    evaluate.mv_scale_x = 1.0f;
    evaluate.mv_scale_y = 1.0f;

    int succeeded = 0;

    for (int frame = 0; frame < frames; ++frame) {
        VkCommandBuffer command = BeginCommands(setup);

        // The model reads colour, motion and depth and writes output, so every frame re-declares the
        // layouts rather than assuming what the last submission left behind.
        Transition(command, color.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
        Transition(command, motion.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
        Transition(command, depth.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT);
        Transition(command, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);

        evaluate.reset = frame == 0 ? 1 : 0;

        if (api->EvaluatePass(command, 0, &evaluate) != 1) {
            Fail("EvaluatePass failed");
            return 4;
        }
        if (!Submit(setup, command)) {
            Fail("the evaluation submission failed");
            return 4;
        }

        ++succeeded;
    }

    std::printf("[info ] %d evaluation(s) submitted\n", succeeded);

    // Read the output back and compare it with the colour that went in.
    {
        VkCommandBuffer command = BeginCommands(setup);

        Transition(command, output.image, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);

        VkBufferImageCopy region{};
        region.bufferOffset = read_offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {size, size, 1};
        vk.cmd_copy_image_to_buffer(command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    staging, 1, &region);

        if (!Submit(setup, command)) {
            Fail("the read-back submission failed");
            return 2;
        }
    }

    const uint16_t* result =
        reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped) + read_offset);
    size_t changed = 0;
    size_t black = 0;
    size_t non_finite = 0;
    double total_difference = 0.0;
    double peak_difference = 0.0;
    double output_luma = 0.0;
    uint16_t sample_in[4] = {};
    uint16_t sample_out[4] = {};

    for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i) {
        double worst = 0.0;

        for (int channel = 0; channel < 3; ++channel) {
            const uint16_t in = colour_half[i * 4 + channel];
            const uint16_t out = result[i * 4 + channel];

            if (IsNonFinite(out)) {
                ++non_finite;
            }

            const double difference =
                std::fabs(static_cast<double>(out) - static_cast<double>(in)) / 1024.0;
            worst = std::max(worst, difference);
        }

        // An output that was never written reads back as zeroes, which would otherwise score as
        // "completely different from the input" and pass. So zeroes are counted separately.
        if ((result[i * 4 + 0] & 0x7FFFu) == 0 && (result[i * 4 + 1] & 0x7FFFu) == 0 &&
            (result[i * 4 + 2] & 0x7FFFu) == 0) {
            ++black;
        }

        output_luma += (HalfToFloat(result[i * 4 + 0]) + HalfToFloat(result[i * 4 + 1]) +
                        HalfToFloat(result[i * 4 + 2])) /
                       3.0;

        total_difference += worst;
        peak_difference = std::max(peak_difference, worst);

        if (worst > 1.0 / 255.0) {
            ++changed;
        }

        if (i == static_cast<size_t>(size) * size / 2 + size / 2) {
            std::memcpy(sample_in, &colour_half[i * 4], sizeof(sample_in));
            std::memcpy(sample_out, &result[i * 4], sizeof(sample_out));
        }
    }

    const double total_pixels = static_cast<double>(size) * size;
    const double changed_fraction = static_cast<double>(changed) / total_pixels;
    const double black_fraction = static_cast<double>(black) / total_pixels;
    const double mean_difference = total_difference / total_pixels;
    const double mean_luma = output_luma / total_pixels;

    std::printf("[info ] changed pixels: %zu / %.0f (%.2f%%)\n", changed, total_pixels,
                changed_fraction * 100.0);
    std::printf("[info ] black pixels: %zu (%.2f%%), non-finite channels: %zu\n", black,
                black_fraction * 100.0, non_finite);
    std::printf("[info ] mean half-unit difference: %.2f, peak: %.2f, output luma: %.3f\n",
                mean_difference, peak_difference, mean_luma);
    std::printf(
        "[info ] centre pixel in 0x%04x 0x%04x 0x%04x 0x%04x -> out 0x%04x 0x%04x 0x%04x 0x%04x\n",
        sample_in[0], sample_in[1], sample_in[2], sample_in[3], sample_out[0], sample_out[1],
        sample_out[2], sample_out[3]);

    // Artifacts for review: raw FP16 in and out, and PPM for anything that opens an image.
    WriteRaw("nr_smoke_input.fp16", colour_half.data(), static_cast<size_t>(colour_bytes));
    WriteRaw("nr_smoke_output.fp16", result, static_cast<size_t>(read_bytes));

    if (WritePpm("nr_smoke_input.ppm", colour_half.data(), size, size) &&
        WritePpm("nr_smoke_output.ppm", result, size, size)) {
        std::printf("[info ] wrote nr_smoke_input.ppm / nr_smoke_output.ppm and matching .fp16\n");
    }

    // Drain before anything is released: freeing a feature or an event that submitted work still
    // references removes the device. The bridge's contract says the caller does this, and the first
    // run of this test did not, which is exactly what a crash at this point looks like.
    vk.device_wait_idle(setup.device);
    api->Shutdown();

    vk.destroy_buffer(setup.device, staging, nullptr);
    vk.unmap_memory(setup.device, staging_memory);
    vk.free_memory(setup.device, staging_memory, nullptr);
    DestroyImage(setup, color);
    DestroyImage(setup, motion);
    DestroyImage(setup, depth);
    DestroyImage(setup, output);
    vk.destroy_command_pool(setup.device, setup.pool, nullptr);
    vk.destroy_device(setup.device, nullptr);
    vk.destroy_instance(setup.instance, nullptr);

    // A model that runs but does not change the picture is the failure that matters, so the verdict
    // is on the pixels. Three ways to fail: nothing changed, the output is unwritten zeroes, or the
    // output is not finite. The 1% bar is deliberately low: the point is "not a no-op".
    //
    // At intensity 0 the picture is supposed to come back unchanged, and --allow-identity says the
    // same for any tuning, so the no-op bar is the wrong test for those runs; the output still has to
    // be written and finite.
    const bool identity_accepted = allow_identity || intensity == 0.0f;

    if (non_finite != 0) {
        Fail("the output contains non-finite values");
        return 5;
    }
    if (black_fraction > 0.99) {
        Fail("the output is almost entirely zeroes; the model never wrote it");
        return 5;
    }
    if (identity_accepted) {
        std::printf("[ pass ] %s, so an unchanged picture is accepted: %.2f%% of pixels differ, "
                    "mean output luma %.3f\n",
                    intensity == 0.0f ? "intensity 0" : "--allow-identity",
                    changed_fraction * 100.0, mean_luma);
        return 0;
    }
    if (changed_fraction < 0.01) {
        Fail("the model ran but the output is effectively the input; the pass is not doing work");
        return 5;
    }

    std::printf("[ pass ] the model changed %.2f%% of pixels, mean output luma %.3f\n",
                changed_fraction * 100.0, mean_luma);
    return 0;
}
