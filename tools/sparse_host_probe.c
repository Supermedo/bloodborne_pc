/* Probe for the PC memory model: guest memory the GPU uses in place, through the sparse arena.
 *
 * Guest direct memory is allocated as host-visible Vulkan memory, exported as a dma-buf and
 * mapped by the runtime (gpu/shim/bbport_guest_memory.cpp). This checks, on the running GPU,
 * whether such memory can back a sparse buffer (the arena) at an arbitrary block, whether CPU
 * writes through the dma-buf mapping are seen by the GPU and GPU writes by the CPU without extra
 * steps, and how fast the GPU reads it compared with device-local memory.
 *
 * Usage: bb-sparse-host-probe [MiB]   (default 256)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            fprintf(stderr, "%s:%d: %s -> %d\n", __FILE__, __LINE__, #x, r_);                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memory_props;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = memory_props.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return i;
    }
    return UINT32_MAX;
}

static void init(void) {
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 8;
    VkPhysicalDevice devices[8];
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t i = 0; i < count && !physical; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical = devices[i];
            printf("GPU: %s (%s)\n", p.deviceName,
                   p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                         : "discrete");
        }
    }
    if (!physical) {
        fprintf(stderr, "no GPU\n");
        exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    uint32_t families = 16;
    VkQueueFamilyProperties fp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fp);
    family = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i) {
        if ((fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fp[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) {
        fprintf(stderr, "no graphics queue with sparse binding\n");
        exit(1);
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
    const char* extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    const VkPhysicalDeviceFeatures features = {.sparseBinding = VK_TRUE,
                                               .sparseResidencyBuffer = VK_TRUE};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 2,
                                    .ppEnabledExtensionNames = extensions,
                                    .pEnabledFeatures = &features};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = family};
    CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
    const VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device, &fci, NULL, &fence));
}

static void begin(void) {
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &bi));
}

static double submit_wait(void) {
    CHECK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd};
    const double start = now_ms();
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    const double ms = now_ms() - start;
    CHECK(vkResetFences(device, 1, &fence));
    return ms;
}

static void barrier(void) {
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                                                 VK_ACCESS_TRANSFER_WRITE_BIT |
                                                 VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

int main(int argc, char** argv) {
    const VkDeviceSize mib = argc > 1 ? strtoull(argv[1], NULL, 10) : 256;
    const VkDeviceSize size = mib << 20, arena_size = 4ull << 30;
    init();

    /* The arena: a 4 GiB sparse buffer, as BufferCache creates it. */
    const VkBufferCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                             VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                                    .size = arena_size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VkBuffer arena;
    CHECK(vkCreateBuffer(device, &sci, NULL, &arena));
    VkMemoryRequirements areq;
    vkGetBufferMemoryRequirements(device, arena, &areq);
    printf("arena: block %llu KiB, memory types 0x%x\n", (unsigned long long)areq.alignment >> 10,
           areq.memoryTypeBits);

    /* Guest memory: host-visible, host-cached, not device-local, exported as a dma-buf. */
    const uint32_t host_type =
        find_type(areq.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (host_type == UINT32_MAX) {
        printf("RESULT: no host-cached memory type can back the arena\n");
        return 2;
    }
    printf("guest memory type %u: flags 0x%x\n", host_type,
           memory_props.memoryTypes[host_type].propertyFlags);
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkMemoryAllocateInfo gai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &export_info,
                                      .allocationSize = size,
                                      .memoryTypeIndex = host_type};
    VkDeviceMemory guest_memory;
    CHECK(vkAllocateMemory(device, &gai, NULL, &guest_memory));
    const PFN_vkGetMemoryFdKHR get_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    const VkMemoryGetFdInfoKHR fdi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                      .memory = guest_memory,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    CHECK(get_fd(device, &fdi, &fd));
    uint8_t* guest = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (guest == MAP_FAILED) {
        perror("mmap dma-buf");
        return 1;
    }

    /* Bind the second half of the guest memory at an arena offset 1 GiB + 3 blocks in. */
    const VkDeviceSize half = size / 2, arena_offset = (1ull << 30) + 3 * areq.alignment;
    const VkSparseMemoryBind bind = {.resourceOffset = arena_offset,
                                     .size = half,
                                     .memory = guest_memory,
                                     .memoryOffset = half};
    const VkSparseBufferMemoryBindInfo bbi = {.buffer = arena, .bindCount = 1, .pBinds = &bind};
    const VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                                  .bufferBindCount = 1,
                                  .pBufferBinds = &bbi};
    CHECK(vkQueueBindSparse(queue, 1, &bsi, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(device, 1, &fence));
    printf("bound %llu MiB of guest memory into the arena\n", (unsigned long long)(half >> 20));

    /* A device-local buffer to copy into. */
    const VkBufferCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = half,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer vram;
    CHECK(vkCreateBuffer(device, &vci, NULL, &vram));
    VkMemoryRequirements vreq;
    vkGetBufferMemoryRequirements(device, vram, &vreq);
    const VkMemoryAllocateInfo vai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vreq.size,
        .memoryTypeIndex = find_type(vreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VkDeviceMemory vram_memory;
    CHECK(vkAllocateMemory(device, &vai, NULL, &vram_memory));
    CHECK(vkBindBufferMemory(device, vram, vram_memory, 0));

    /* 1. CPU writes through the dma-buf mapping, the GPU copies through the arena. */
    uint32_t* words = (uint32_t*)(guest + half);
    for (VkDeviceSize i = 0; i < half / 4; ++i) words[i] = (uint32_t)(i * 2654435761u);
    begin();
    const VkBufferCopy to_vram = {.srcOffset = arena_offset, .dstOffset = 0, .size = half};
    vkCmdCopyBuffer(cmd, arena, vram, 1, &to_vram);
    barrier();
    /* 2. The GPU writes through the arena, the CPU reads through the mapping. */
    vkCmdFillBuffer(cmd, arena, arena_offset + 4096, 4096, 0xabcd1234u);
    barrier();
    const VkBufferCopy back = {.srcOffset = 0, .dstOffset = arena_offset + 8192, .size = 4096};
    vkCmdCopyBuffer(cmd, vram, arena, 1, &back);
    barrier();
    submit_wait();
    int ok = 1;
    for (int i = 0; i < 1024; ++i) ok &= words[1024 + i] == 0xabcd1234u;
    for (int i = 0; i < 1024; ++i) ok &= words[2048 + i] == (uint32_t)(i * 2654435761u);
    printf("CPU write -> GPU read -> GPU write -> CPU read: %s\n", ok ? "ok" : "MISMATCH");

    /* 3. GPU read bandwidth: guest memory through the arena vs device-local memory. */
    double host_ms = 1e9, vram_ms = 1e9;
    for (int run = 0; run < 5; ++run) {
        begin();
        vkCmdCopyBuffer(cmd, arena, vram, 1, &to_vram);
        const double h = submit_wait();
        host_ms = h < host_ms ? h : host_ms;
        begin();
        const VkBufferCopy within = {.srcOffset = 0, .dstOffset = half / 2, .size = half / 2};
        vkCmdCopyBuffer(cmd, vram, vram, 1, &within);
        const double v = submit_wait() * 2; /* the same bytes as the copy above */
        vram_ms = v < vram_ms ? v : vram_ms;
    }
    printf("GPU reads %llu MiB: guest memory %.2f ms (%.1f GB/s), device-local %.2f ms (%.1f GB/s)\n",
           (unsigned long long)(half >> 20), host_ms, half / host_ms / 1e6, vram_ms,
           half / vram_ms / 1e6);
    printf("RESULT: %s\n", ok ? "guest memory can back the arena" : "data mismatch");
    return ok ? 0 : 1;
}
