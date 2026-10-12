/* Probe for importing guest memory into Vulkan (VK_EXT_external_memory_host).
 *
 * Guest memory is a memfd mapped twice: at guest addresses (page protection for write tracking)
 * and as an unprotected backing view. This checks, on the running GPU, whether the backing view
 * of a memfd can be imported, how fast the GPU reads it compared with today's path (CPU copy
 * into a staging buffer, then a GPU copy), whether CPU writes through the other mapping are
 * seen without extra steps, and what mprotect on the guest view costs the imported memory.
 *
 * Usage: bb-host-import-probe [MiB]   (default 512)
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
static PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_props;

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
    physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            physical = devices[i];
            printf("GPU: %s\n", p.deviceName);
            break;
        }
    }
    if (!physical) {
        fprintf(stderr, "no GPU\n");
        exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_props);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &host_props};
    vkGetPhysicalDeviceProperties2(physical, &props2);
    printf("minImportedHostPointerAlignment: %llu\n",
           (unsigned long long)host_props.minImportedHostPointerAlignment);
    family = 0;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
    const char* extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = 1};
    const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &f13,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 3,
                                    .ppEnabledExtensionNames = extensions};
    CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    get_host_pointer_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
        device, "vkGetMemoryHostPointerPropertiesEXT");
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

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
} Buffer;

static Buffer make_buffer(VkDeviceSize size, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    Buffer b = {0};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(device, &bci, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .allocationSize = req.size,
                                      .memoryTypeIndex = find_type(req.memoryTypeBits, want, avoid)};
    CHECK(vkAllocateMemory(device, &mai, NULL, &b.memory));
    CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    }
    return b;
}

/* Imports [pointer, pointer + size) as a transfer buffer; returns 0 on failure. */
static int import_buffer(void* pointer, VkDeviceSize size, Buffer* out, double* ms) {
    VkMemoryHostPointerPropertiesEXT hpp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = get_host_pointer_props(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &hpp);
    if (r != VK_SUCCESS || !hpp.memoryTypeBits) {
        printf("  host pointer properties: result %d, memory types %#x\n", r, hpp.memoryTypeBits);
        return 0;
    }
    const VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .pNext = &ext,
                                    .size = size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(device, &bci, NULL, &out->buffer));
    const VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = pointer};
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i) {
        if (hpp.memoryTypeBits & (1u << i)) {
            type = i;
            break;
        }
    }
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &import,
                                      .allocationSize = size,
                                      .memoryTypeIndex = type};
    const double start = now_ms();
    r = vkAllocateMemory(device, &mai, NULL, &out->memory);
    *ms = now_ms() - start;
    if (r != VK_SUCCESS) {
        printf("  vkAllocateMemory(import) failed: %d\n", r);
        vkDestroyBuffer(device, out->buffer, NULL);
        return 0;
    }
    printf("  memory type %u (flags %#x)\n", type, memory_props.memoryTypes[type].propertyFlags);
    CHECK(vkBindBufferMemory(device, out->buffer, out->memory, 0));
    out->mapped = pointer;
    return 1;
}

/* Records `copies` copies of `size` bytes src -> dst, submits, waits; returns wall ms. */
static double gpu_copy(VkBuffer src, VkDeviceSize src_offset, VkBuffer dst, VkDeviceSize size) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    const VkBufferCopy region = {.srcOffset = src_offset, .dstOffset = 0, .size = size};
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &barrier, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &cmd};
    const double start = now_ms();
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    const double ms = now_ms() - start;
    CHECK(vkResetFences(device, 1, &fence));
    return ms;
}

#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>

static volatile sig_atomic_t write_faults;
static uint8_t* fault_view;
static uint64_t fault_size;
static void on_write_fault(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)context;
    uint8_t* address = info->si_addr;
    if (address >= fault_view && address < fault_view + fault_size) {
        ++write_faults;
        uint8_t* page = (uint8_t*)((uintptr_t)address & ~(uintptr_t)4095);
        mprotect(page, 4096, PROT_READ | PROT_WRITE);
        return;
    }
    signal(SIGSEGV, SIG_DFL);
}

/* CPU speed on a mapping: sequential write, sequential read, random 8-byte reads. */
static void cpu_speed(const char* name, uint8_t* p, uint64_t size) {
    double start = now_ms();
    memset(p, 0x5a, size);
    const double write_ms = now_ms() - start;
    start = now_ms();
    uint64_t sum = 0;
    for (uint64_t i = 0; i < size; i += 64) sum += p[i];
    const double read_ms = now_ms() - start;
    start = now_ms();
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 4000000; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        sum += p[(x % (size / 8)) * 8];
    }
    const double random_ms = now_ms() - start;
    printf("  %-28s write %6.1f GB/s, read %6.1f GB/s, random reads %6.1f ns (%llu)\n", name,
           size / write_ms / 1e6, size / read_ms / 1e6, random_ms * 1e6 / 4000000,
           (unsigned long long)(sum & 1));
}

/* Guest memory allocated by Vulkan (system memory the GPU reaches), exported as a dma-buf and
 * mapped by the CPU: no import, several views of the same memory, protection on the CPU views
 * only. */
static void dmabuf_test(uint64_t size, Buffer local, Buffer readback) {
    printf("\n5. Vulkan-allocated guest memory exported as dma-buf (%llu MiB)\n",
           (unsigned long long)(size >> 20));
    PFN_vkGetMemoryFdKHR get_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    const VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .pNext = &ext,
                                    .size = size,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const uint32_t type =
        find_type(req.memoryTypeBits,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        printf("  no cached system memory type for this buffer\n");
        return;
    }
    const VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                      .pNext = &export_info,
                                      .allocationSize = req.size,
                                      .memoryTypeIndex = type};
    VkDeviceMemory memory;
    double start = now_ms();
    VkResult r = vkAllocateMemory(device, &mai, NULL, &memory);
    printf("  allocate (type %u, flags %#x): %d, %.1f ms\n", type,
           memory_props.memoryTypes[type].propertyFlags, r, now_ms() - start);
    if (r != VK_SUCCESS) return;
    CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    const VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                                      .memory = memory,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    r = get_fd(device, &gfi, &fd);
    printf("  export dma-buf fd: %d (fd %d)\n", r, fd);
    if (r != VK_SUCCESS) return;
    uint8_t* guest = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t* backing = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (guest == MAP_FAILED || backing == MAP_FAILED) {
        perror("  mmap of the dma-buf");
        return;
    }
    printf("  two CPU views mapped: OK\n");

    uint8_t* anon = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    cpu_speed("ordinary memory", anon, size);
    const int shm = memfd_create("bb-probe-speed", MFD_CLOEXEC);
    if (shm >= 0 && !ftruncate(shm, (off_t)size)) {
        uint8_t* view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
        if (view != MAP_FAILED) cpu_speed("memfd (the guest memory now)", view, size);
    }
    cpu_speed("dma-buf guest view", guest, size);

    /* A view of part of it at a chosen address (guest mappings: MAP_FIXED at an offset). */
    uint8_t* hole = mmap(NULL, 8 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t* fixed = mmap(hole + (1 << 20), 4 << 20, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                          fd, (off_t)(256 << 20));
    if (fixed == MAP_FAILED) {
        perror("  MAP_FIXED view at an offset");
    } else {
        guest[(256 << 20) + 64] = 0x42;
        printf("  MAP_FIXED view at offset 256 MiB: %s\n", fixed[64] == 0x42 ? "OK" : "WRONG DATA");
    }
    /* Aliasing: a write through one view is visible through the other. */
    guest[12345] = 0x77;
    printf("  aliasing: %s\n", backing[12345] == 0x77 ? "OK" : "BROKEN");

    const VkDeviceSize chunk = 64ull << 20 < size ? 64ull << 20 : size;
    double best = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(buffer, 0, local.buffer, chunk);
        if (ms < best) best = ms;
    }
    printf("  GPU reads %llu MiB of it: %.2f ms (%.1f GB/s)\n", (unsigned long long)(chunk >> 20),
           best, chunk / best / 1e6);

    int bad = 0;
    for (int round = 0; round < 64; ++round) {
        const uint64_t offset = ((uint64_t)rand() % (size / 4096)) * 4096;
        const uint32_t value = 0x51ac0000u + (uint32_t)round;
        memcpy(guest + offset, &value, 4);
        gpu_copy(buffer, offset, readback.buffer, 4);
        uint32_t seen;
        memcpy(&seen, readback.mapped, 4);
        if (seen != value) ++bad;
    }
    printf("  coherence (CPU write, GPU read): %s (%d of 64 stale)\n", bad ? "STALE" : "OK", bad);

    double base = 1e9, after = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(buffer, 0, local.buffer, 4 << 20);
        if (ms < base) base = ms;
    }
    start = now_ms();
    for (int i = 0; i < 1000; ++i) {
        uint8_t* page = guest + ((uint64_t)i * 65536) % size;
        mprotect(page, 65536, PROT_READ);
        mprotect(page, 65536, PROT_READ | PROT_WRITE);
    }
    const double protect_ms = now_ms() - start;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(buffer, 0, local.buffer, 4 << 20);
        if (ms < after) after = ms;
    }
    printf("  2000 mprotect calls on the guest view: %.1f ms; 4 MiB GPU read before %.3f ms, "
           "after %.3f ms\n", protect_ms, base, after);

    /* Write tracking as the GPU caches do it: protect, write, catch the fault, unprotect. */
    fault_view = guest;
    fault_size = size;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_write_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    mprotect(guest, 1 << 20, PROT_READ);
    for (int i = 0; i < 256; ++i) guest[i * 4096] = 1;
    printf("  write faults caught after mprotect: %d of 256\n", (int)write_faults);

    /* File reads into the guest view (the game loads its data with read()). */
    char path[] = "/tmp/bb-probe-file-XXXXXX";
    const int file = mkstemp(path);
    static char data[1 << 20];
    memset(data, 0x3c, sizeof(data));
    if (file >= 0 && write(file, data, sizeof(data)) == (ssize_t)sizeof(data)) {
        const ssize_t got = pread(file, guest + (2 << 20), sizeof(data), 0);
        printf("  read() into the guest view: %zd bytes, %s\n", got,
               got == (ssize_t)sizeof(data) && guest[(2 << 20) + 100] == 0x3c ? "OK" : "FAILED");
        close(file);
        unlink(path);
    }
}

int main(int argc, char** argv) {
    const uint64_t mib = argc > 1 ? strtoull(argv[1], NULL, 10) : 512;
    const uint64_t size = mib << 20;
    init();

    /* Guest memory as the runtime makes it: a memfd, a backing view, a guest view. */
    const int fd = memfd_create("bb-probe-guest", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size)) {
        perror("memfd");
        return 1;
    }
    uint8_t* backing = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    uint8_t* guest = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (backing == MAP_FAILED || guest == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    for (uint64_t i = 0; i < size; i += 4096) guest[i] = (uint8_t)(i >> 12);

    printf("\n1. Import of the memfd backing view (%llu MiB)\n", (unsigned long long)mib);
    Buffer imported;
    double import_ms = 0;
    const int memfd_ok = import_buffer(backing, size, &imported, &import_ms);
    printf("  %s (%.1f ms)\n", memfd_ok ? "OK" : "FAILED", import_ms);

    printf("\n1b. Import of anonymous memory (%llu MiB), for comparison\n", (unsigned long long)mib);
    uint8_t* anon = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(anon, 1, size);
    Buffer anon_buffer;
    double anon_ms = 0;
    const int anon_ok = import_buffer(anon, size, &anon_buffer, &anon_ms);
    printf("  %s (%.1f ms)\n", anon_ok ? "OK" : "FAILED", anon_ms);

    if (!memfd_ok) {
        if (!anon_ok) {
            printf("\nNeither memfd nor anonymous memory can be imported: stopping.\n");
            return 2;
        }
        // The rest on anonymous memory: there is no second view, so the "guest view" is the
        // imported range itself (protection changes hit the imported pages).
        printf("\nmemfd import failed: the tests below use the anonymous import.\n");
        imported = anon_buffer;
        backing = anon;
        guest = anon;
    }

    const VkDeviceSize chunk = 64ull << 20 < size ? 64ull << 20 : size;
    Buffer local = make_buffer(chunk, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    Buffer staging = make_buffer(chunk, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Buffer readback = make_buffer(4096, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);

    printf("\n2. Upload of %llu MiB into device-local memory\n", (unsigned long long)(chunk >> 20));
    gpu_copy(imported.buffer, 0, local.buffer, chunk); /* warm up */
    double direct = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.buffer, (uint64_t)i * 4096 % (size - chunk + 1),
                                   local.buffer, chunk);
        if (ms < direct) direct = ms;
    }
    double cpu = 1e9, gpu = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double start = now_ms();
        memcpy(staging.mapped, guest, chunk);
        const double c = now_ms() - start;
        const double g = gpu_copy(staging.buffer, 0, local.buffer, chunk);
        if (c < cpu) cpu = c;
        if (g < gpu) gpu = g;
    }
    printf("  GPU reads the imported guest memory: %.2f ms (%.1f GB/s)\n", direct,
           chunk / direct / 1e6);
    printf("  today: CPU copy to staging %.2f ms + GPU copy %.2f ms = %.2f ms (CPU busy %.2f ms)\n",
           cpu, gpu, cpu + gpu, cpu);

    printf("\n3. Coherence: CPU writes through the guest view, GPU reads the imported view\n");
    int bad = 0;
    for (int round = 0; round < 64; ++round) {
        const uint64_t offset = ((uint64_t)rand() % (size / 4096)) * 4096;
        const uint32_t value = 0x51ac0000u + (uint32_t)round;
        memcpy(guest + offset, &value, 4);
        gpu_copy(imported.buffer, offset, readback.buffer, 4);
        uint32_t seen;
        memcpy(&seen, readback.mapped, 4);
        if (seen != value) ++bad;
    }
    printf("  %s (%d of 64 rounds stale)\n", bad ? "STALE DATA" : "OK", bad);

    printf("\n4. mprotect on the guest view vs the imported backing view\n");
    double base = 1e9, after = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.buffer, 0, local.buffer, 4 << 20);
        if (ms < base) base = ms;
    }
    const double protect_start = now_ms();
    for (int i = 0; i < 1000; ++i) {
        uint8_t* page = guest + ((uint64_t)i * 65536) % size;
        mprotect(page, 65536, PROT_READ);
        mprotect(page, 65536, PROT_READ | PROT_WRITE);
    }
    const double protect_ms = now_ms() - protect_start;
    for (int i = 0; i < 5; ++i) {
        const double ms = gpu_copy(imported.buffer, 0, local.buffer, 4 << 20);
        if (ms < after) after = ms;
    }
    printf("  2000 mprotect calls on the guest view: %.1f ms; 4 MiB GPU read before %.3f ms, "
           "after %.3f ms\n", protect_ms, base, after);
    /* The same on the imported view itself (what to avoid). */
    double own = 0;
    for (int i = 0; i < 3; ++i) {
        mprotect(backing, 65536, PROT_READ);
        mprotect(backing, 65536, PROT_READ | PROT_WRITE);
        own += gpu_copy(imported.buffer, 0, local.buffer, 4 << 20);
    }
    printf("  mprotect on the imported view itself, then a 4 MiB GPU read: %.3f ms average\n",
           own / 3);

    dmabuf_test(size, local, readback);
    printf("\nDone.\n");
    return 0;
}
