#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <vulkan/vulkan.h>
#include "bbport_dlss_bridge.h"

static void log_line(int warning, const char *message) {
    printf("bridge: %s%s\n", warning ? "warning: " : "", message);
}

static VkPhysicalDevice pick_nvidia(VkInstance instance) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) return NULL;
    VkPhysicalDevice *list = calloc(count, sizeof(*list));
    if (!list || vkEnumeratePhysicalDevices(instance, &count, list) != VK_SUCCESS) {
        free(list);
        return NULL;
    }
    VkPhysicalDevice chosen = NULL;
    for (uint32_t i = 0; i < count && !chosen; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(list[i], &props);
        if (props.vendorID == 0x10de) {
            chosen = list[i];
            printf("GPU: %s, driver 0x%08x\n", props.deviceName, props.driverVersion);
        }
    }
    free(list);
    return chosen;
}

int main(int argc, char **argv) {
    wchar_t directory[MAX_PATH];
    if (argc > 1) {
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, directory, MAX_PATH);
    } else {
        const DWORD length = GetModuleFileNameW(NULL, directory, MAX_PATH);
        for (DWORD i = length; i > 0; --i) {
            if (directory[i - 1] == L'\\' || directory[i - 1] == L'/') {
                directory[i - 1] = 0;
                break;
            }
        }
    }
    wchar_t bridge_path[MAX_PATH];
    swprintf(bridge_path, MAX_PATH, L"%ls\\bbport_dlss.dll", directory);
    HMODULE module = LoadLibraryW(bridge_path);
    if (!module) {
        printf("bbport_dlss.dll not found in %ls\n", directory);
        return 2;
    }
    BbDlssGetApiFn get_api = (BbDlssGetApiFn)(void (*)(void))GetProcAddress(module, "BbDlssGetApi");
    const BbDlssApi *api = get_api ? get_api() : NULL;
    if (!api) {
        printf("BbDlssGetApi missing\n");
        return 2;
    }
    printf("bridge ABI %u\n", api->abi);
    wchar_t data[MAX_PATH];
    swprintf(data, MAX_PATH, L"%ls\\dlss", directory);
    CreateDirectoryW(data, NULL);
    if (!api->Configure(directory, data, log_line)) {
        printf("Configure failed\n");
        return 2;
    }
    uint32_t instance_count = 0;
    const VkExtensionProperties *instance_extensions = NULL;
    if (!api->InstanceExtensions(&instance_count, &instance_extensions)) {
        printf("InstanceExtensions failed\n");
        return 2;
    }
    const char **names = calloc(instance_count + 1, sizeof(*names));
    for (uint32_t i = 0; i < instance_count; ++i) names[i] = instance_extensions[i].extensionName;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "bb-dlss-capabilities",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   .pApplicationInfo = &app,
                                   .enabledExtensionCount = instance_count,
                                   .ppEnabledExtensionNames = names};
    VkInstance instance;
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n");
        return 2;
    }
    VkPhysicalDevice physical = pick_nvidia(instance);
    if (!physical) {
        printf("no NVIDIA GPU\n");
        return 2;
    }
    uint32_t device_count = 0;
    const VkExtensionProperties *device_extensions = NULL;
    if (!api->DeviceExtensions(instance, physical, &device_count, &device_extensions)) {
        printf("DeviceExtensions failed (DLSS unsupported)\n");
        return 2;
    }
    const char **device_names = calloc(device_count + 1, sizeof(*device_names));
    for (uint32_t i = 0; i < device_count; ++i) {
        device_names[i] = device_extensions[i].extensionName;
        printf("device extension: %s\n", device_names[i]);
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = 0,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &features12};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                      .pNext = &features,
                                      .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue,
                                      .enabledExtensionCount = device_count,
                                      .ppEnabledExtensionNames = device_names};
    VkDevice device;
    if (vkCreateDevice(physical, &device_info, NULL, &device) != VK_SUCCESS) {
        printf("vkCreateDevice failed\n");
        return 2;
    }
    if (!api->Initialize(instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr)) {
        printf("NGX initialisation failed\n");
        return 1;
    }
    printf("DLSS Super Resolution: yes\n");
    const int fg = api->abi >= 2 ? api->FrameGenMaxCount() : 0;
    printf("DLSS Frame Generation: %s (max %d generated frames per pair)\n", fg ? "yes" : "no",
           fg);
    const int rr = api->abi >= 3 ? api->RayReconstructionAvailable() : 0;
    printf("DLSS Ray Reconstruction: %s\n", rr ? "yes" : "no");
    api->Shutdown();
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
