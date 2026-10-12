// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Checks DLSS without the game: loads libbbport_dlss.so from the given directory (default: next
// to this program), creates a Vulkan device with the extensions NGX asks for, creates a DLSS
// feature (1280x720 -> 2560x1440) and evaluates one frame. Exit status 0 when all of it works.
//   tools/build_dlss_linux.sh && out/bb-dlss-selftest

#define _GNU_SOURCE
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#include "gpu/dlss_bridge/bbport_dlss_bridge.h"

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        VkResult result_ = (expression);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            fprintf(stderr, "FAIL: %s = %d\n", #expression, result_);                              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void Log(int warning, const char* message) {
    printf("  bridge: %s%s\n", warning ? "warning: " : "", message);
}

static int Contains(const char** names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(names[i], name) == 0) return 1;
    }
    return 0;
}

static VkDevice device;
static VkPhysicalDevice physical;

typedef struct Image {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    BbDlssImage bridge;
} Image;

static Image MakeImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect) {
    Image out = {0};
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(device, &info, NULL, &out.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = 0;
    for (; type < memory.memoryTypeCount; ++type) {
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    }
    const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .allocationSize = requirements.size,
                                           .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(device, &allocate, NULL, &out.memory));
    CHECK(vkBindImageMemory(device, out.image, out.memory, 0));
    const VkImageSubresourceRange range = {aspect, 0, 1, 0, 1};
    const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        .image = out.image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = format, .subresourceRange = range};
    CHECK(vkCreateImageView(device, &view, NULL, &out.view));
    out.bridge = (BbDlssImage){out.image, out.view, range, format, width, height};
    return out;
}

static void Transition(VkCommandBuffer command, const Image* image, VkImageLayout layout) {
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image->image,
        .subresourceRange = image->bridge.range};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

int main(int argc, char** argv) {
    char directory[PATH_MAX];
    if (argc > 1) {
        snprintf(directory, sizeof directory, "%s", argv[1]);
    } else {
        char self[PATH_MAX] = {0};
        if (readlink("/proc/self/exe", self, sizeof self - 1) < 0) return 1;
        snprintf(directory, sizeof directory, "%s", dirname(self));
    }
    char bridge_path[PATH_MAX + 32];
    snprintf(bridge_path, sizeof bridge_path, "%s/libbbport_dlss.so", directory);
    void* module = dlopen(bridge_path, RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "FAIL: %s\n", dlerror());
        return 1;
    }
    const BbDlssGetApiFn get_api = (BbDlssGetApiFn)dlsym(module, "BbDlssGetApi");
    const BbDlssApi* api = get_api ? get_api() : NULL;
    if (!api || api->abi != BBPORT_DLSS_BRIDGE_ABI) {
        fprintf(stderr, "FAIL: bridge ABI mismatch\n");
        return 1;
    }
    printf("Bridge: %s\n", bridge_path);

    wchar_t wide_directory[PATH_MAX], wide_data[PATH_MAX];
    char data[PATH_MAX + 16];
    snprintf(data, sizeof data, "%s/dlss", directory);
    mkdir(data, 0755);
    mbstowcs(wide_directory, directory, PATH_MAX);
    mbstowcs(wide_data, data, PATH_MAX);
    if (!api->Configure(wide_directory, wide_data, Log)) return 1;

    // Instance: everything NGX needs.
    uint32_t count = 0;
    const VkExtensionProperties* required = NULL;
    if (!api->InstanceExtensions(&count, &required)) return 1;
    const char* instance_extensions[64];
    uint32_t instance_count = 0;
    for (uint32_t i = 0; i < count && instance_count < 64; ++i) {
        instance_extensions[instance_count++] = required[i].extensionName;
    }
    printf("Instance extensions NGX needs: %u\n", count);
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "bb-dlss-selftest",
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &app,
                                                .enabledExtensionCount = instance_count,
                                                .ppEnabledExtensionNames = instance_extensions};
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));

    uint32_t physical_count = 8;
    VkPhysicalDevice physicals[8];
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, physicals));
    physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < physical_count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physicals[i], &properties);
        if (properties.vendorID == 0x10de) {
            physical = physicals[i];
            printf("GPU: %s\n", properties.deviceName);
            break;
        }
    }
    if (!physical) {
        fprintf(stderr, "FAIL: no NVIDIA GPU\n");
        return 1;
    }
    if (!api->DeviceExtensions(instance, physical, &count, &required)) {
        fprintf(stderr, "FAIL: DLSS not supported by this GPU/driver\n");
        return 1;
    }
    const char* device_extensions[64];
    uint32_t device_count = 0;
    for (uint32_t i = 0; i < count && device_count < 64; ++i) {
        if (!Contains(device_extensions, device_count, required[i].extensionName))
            device_extensions[device_count++] = required[i].extensionName;
    }
    printf("Device extensions NGX needs: %u\n", count);

    uint32_t family_count = 16;
    VkQueueFamilyProperties families[16];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = family, .queueCount = 1,
                                                .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = 1};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                            .pNext = &features12, .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &queue_info,
                                            .enabledExtensionCount = device_count,
                                            .ppEnabledExtensionNames = device_extensions};
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    if (!api->Initialize(instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr)) {
        fprintf(stderr, "FAIL: NGX initialization\n");
        return 1;
    }
    printf("NGX initialized\n");

    const uint32_t w = 1280, h = 720, ow = 2560, oh = 1440;
    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    Image color = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, w, h, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = MakeImage(VK_FORMAT_D32_SFLOAT, w, h, sampled, VK_IMAGE_ASPECT_DEPTH_BIT);
    Image motion = MakeImage(VK_FORMAT_R16G16_SFLOAT, w, h, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
    Image output = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh,
                             VK_IMAGE_USAGE_STORAGE_BIT | sampled, VK_IMAGE_ASPECT_COLOR_BIT);

    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .queueFamilyIndex = family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    Transition(command, &color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(command, &depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(command, &motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(command, &output, VK_IMAGE_LAYOUT_GENERAL);
    const BbDlssFeature feature = {w, h, ow, oh, 1, 0, 0, 0};
    if (!api->CreateFeature(command, &feature)) {
        fprintf(stderr, "FAIL: DLSS feature creation\n");
        return 1;
    }
    const BbDlssEvaluate evaluate = {color.bridge, depth.bridge, motion.bridge, output.bridge,
                                     0.25f, -0.25f, 1, 16.6f, 0.0f};
    if (!api->Evaluate(command, &evaluate)) {
        fprintf(stderr, "FAIL: DLSS evaluation\n");
        return 1;
    }
    CHECK(vkEndCommandBuffer(command));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    printf("DLSS feature created and one frame evaluated (%ux%u -> %ux%u)\n", w, h, ow, oh);
    api->ReleaseFeature();
    api->Shutdown();
    printf("PASS: DLSS works on this system\n");
    return 0;
}
