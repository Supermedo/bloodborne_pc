// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_memory.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys,
                                                                      uint64_t size));
extern "C" void runtime_memory_set_guest_chunk_whole(int whole);

namespace BbGuestMemory {
namespace {
using u64 = std::uint64_t;
constexpr std::size_t MaxChunks = 256;

vk::Device device;
vk::PhysicalDeviceMemoryProperties memory_properties;
std::mutex mutex;
std::array<Chunk*, MaxChunks> chunks{}; // sorted by phys
std::size_t chunk_count = 0;
bool whole_only = false; // the driver's dma-buf maps at offset 0 only: a chunk per allocation
std::atomic<u64> chunk_bytes{0};

std::uint32_t FindType(std::uint32_t bits) {
    const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (flags & want) == want &&
            !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return i;
        }
    }
    return ~0u;
}

/// The runtime's chunk allocator: a dma-buf fd of `size` bytes of GPU-visible memory, or -1.
int AllocChunk(u64 phys, u64 size) {
    {
        std::scoped_lock lk{mutex};
        if (chunk_count == MaxChunks) {
            return -1;
        }
    }
    static std::atomic<int> failures{0};
    const auto fail = [&](const char* what, vk::Result result) {
        if (failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: %s failed (%s); chunk %#llx stays in the memfd\n",
                         what, vk::to_string(result).c_str(), (unsigned long long)phys);
        }
        return -1;
    };
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const vk::BufferCreateInfo buffer_ci{
        .pNext = &external,
        .size = size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
                 vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eIndirectBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto [buffer_result, buffer] = device.createBuffer(buffer_ci);
    if (buffer_result != vk::Result::eSuccess) {
        return fail("buffer creation", buffer_result);
    }
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    const std::uint32_t type = FindType(requirements.memoryTypeBits);
    if (type == ~0u) {
        device.destroyBuffer(buffer);
        return fail("finding a cached system memory type", vk::Result::eErrorFeatureNotPresent);
    }
    const vk::ExportMemoryAllocateInfo export_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        device.destroyBuffer(buffer);
        return fail("allocation", memory_result);
    }
    if (const auto result = device.bindBufferMemory(buffer, memory, 0);
        result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        return fail("binding", result);
    }
    const auto [fd_result, fd] = device.getMemoryFdKHR({
        .memory = memory,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
    });
    if (fd_result != vk::Result::eSuccess) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        return fail("dma-buf export", fd_result);
    }
    {
        std::scoped_lock lk{mutex};
        std::size_t i = chunk_count++;
        for (; i > 0 && chunks[i - 1]->phys > phys; --i) {
            chunks[i] = chunks[i - 1];
        }
        static std::uint32_t next_index = 0;
        chunks[i] = new Chunk{phys, size, buffer, memory, next_index++};
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    std::printf("Guest memory: direct memory %#llx+%llu MiB in GPU-visible memory (dma-buf), "
                "%llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(size >> 20),
                (unsigned long long)(total >> 20));
    return fd;
}
} // namespace

bool PcModelGpu(const Vulkan::Instance& instance) {
    static const bool ok = [&] {
        constexpr std::uint32_t AmdVendor = 0x1002;
        const std::uint32_t vendor = instance.GetVendorID();
        const char* any = std::getenv("BB_PC_MODEL_ANY_GPU");
        if (vendor == AmdVendor || (any && any[0] == '1')) {
            return true;
        }
        std::printf("Guest memory: the new memory model is tested on AMD GPUs only; this GPU "
                    "(vendor 0x%04x) uses the model of 0.3 (BB_PC_MODEL_ANY_GPU=1: try it)\n",
                    vendor);
        return false;
    }();
    return ok;
}

bool Usable(const Vulkan::Instance& instance) {
    static const bool usable = [&] {
        if (!instance.IsGuestMemoryExportSupported()) {
            std::printf("Guest memory: the driver cannot export system memory as a dma-buf\n");
            return false;
        }
        const vk::Device dev = instance.GetDevice();
        const vk::PhysicalDeviceMemoryProperties props = instance.GetPhysicalDevice().getMemoryProperties();
        constexpr u64 Size = 2 << 20, Offset = 1 << 20;
        const vk::ExternalMemoryBufferCreateInfo external{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        const auto [buffer_result, buffer] = dev.createBuffer({
            .pNext = &external,
            .size = Size,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        if (buffer_result != vk::Result::eSuccess) {
            return false;
        }
        const auto requirements = dev.getBufferMemoryRequirements(buffer);
        std::uint32_t type = ~0u;
        const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i) {
            const auto flags = props.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & want) == want &&
                !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                type = i;
            }
        }
        bool ok = false;
        vk::DeviceMemory memory{};
        int fd = -1;
        const vk::ExportMemoryAllocateInfo export_info{
            .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
        };
        if (type != ~0u) {
            const auto [memory_result, allocated] = dev.allocateMemory(
                {.pNext = &export_info, .allocationSize = requirements.size, .memoryTypeIndex = type});
            if (memory_result == vk::Result::eSuccess) {
                memory = allocated;
                const auto [fd_result, exported] = dev.getMemoryFdKHR(
                    {.memory = memory, .handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT});
                fd = fd_result == vk::Result::eSuccess ? exported : -1;
            }
        }
        if (fd >= 0) {
            // The whole buffer and one page inside it, writing through one, reading through the other.
            void* whole = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Offset);
            // BB_GUEST_WHOLE_CHUNKS=1: as if the offset mapping failed (tests the NVIDIA path).
            const char* force_whole = std::getenv("BB_GUEST_WHOLE_CHUNKS");
            if (force_whole && force_whole[0] == '1' && page != MAP_FAILED) {
                munmap(page, 4096);
                page = MAP_FAILED;
            }
            if (whole != MAP_FAILED && page != MAP_FAILED) {
                *static_cast<volatile std::uint32_t*>(page) = 0x5ca1ab1e;
                ok = static_cast<volatile std::uint32_t*>(whole)[Offset / 4] == 0x5ca1ab1e;
            } else if (whole != MAP_FAILED) {
                // Offset 0 only (NVIDIA): a second mapping of the start must alias the first.
                void* again = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (again != MAP_FAILED) {
                    *static_cast<volatile std::uint32_t*>(again) = 0x5ca1ab1e;
                    ok = whole_only = static_cast<volatile std::uint32_t*>(whole)[0] == 0x5ca1ab1e;
                    munmap(again, 4096);
                }
            }
            if (whole != MAP_FAILED) {
                munmap(whole, Size);
            }
            if (page != MAP_FAILED) {
                munmap(page, 4096);
            }
            close(fd);
        }
        if (memory) {
            dev.freeMemory(memory);
        }
        dev.destroyBuffer(buffer);
        if (!ok) {
            std::printf("Guest memory: the driver's dma-buf cannot be mapped (%s)\n",
                        fd >= 0 ? "mmap failed" : "no exportable cached system memory");
        } else if (whole_only) {
            std::printf("Guest memory: the driver's dma-buf maps at offset 0 only: one chunk per "
                        "direct memory allocation\n");
        }
        return ok;
    }();
    return usable;
}

void Install(const Vulkan::Instance& instance) {
    // BB_GUEST_IN_PLACE (the GPU uses this memory in place) needs it too.
    const char* env = std::getenv("BB_GUEST_GPU_MEMORY");
    const char* in_place = std::getenv("BB_GUEST_IN_PLACE");
    if (!(env && env[0] == '1') && !(in_place && in_place[0] == '1' && PcModelGpu(instance))) {
        return;
    }
    if (!Usable(instance)) {
        std::printf("Guest memory: direct memory stays in the memfd\n");
        return;
    }
    device = instance.GetDevice();
    memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    runtime_memory_set_guest_chunk_whole(whole_only ? 1 : 0);
    runtime_memory_set_guest_chunk_allocator(&AllocChunk);
    std::printf("Guest memory: direct memory chunks come from Vulkan (BB_GUEST_GPU_MEMORY=1)\n");
}

const Chunk* Find(std::uint64_t phys) {
    std::scoped_lock lk{mutex};
    // The last chunk starting at or below phys.
    std::size_t lo = 0, hi = chunk_count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (chunks[mid]->phys <= phys) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return nullptr;
    }
    const Chunk* c = chunks[lo - 1];
    return phys < c->phys + c->size ? c : nullptr;
}
} // namespace BbGuestMemory
