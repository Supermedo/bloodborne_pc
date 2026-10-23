// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_fsr411.h"
#include "driver_provider.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

namespace {
std::filesystem::path ModuleDirectory() {
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(n);
    return std::filesystem::path(path).parent_path();
}
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char error[200];
        std::snprintf(error, sizeof(error), "%s: HRESULT 0x%08lx", operation, (unsigned long)hr);
        throw std::runtime_error(error);
    }
}
template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
uint32_t ApiFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    default: throw std::runtime_error("unsupported FSR 4.1.1 input format");
    }
}
}

struct NativeFsr411::Impl {
    NativeFsr411Config config;
    HMODULE module{};
    ffxFunctions ffx{};
    ffxContext context{};
    IDXGIFactory4* factory{};
    IDXGIAdapter1* adapter{};
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    struct Slot {
        ID3D12CommandAllocator* allocator{};
        ID3D12GraphicsCommandList* list{};
        uint64_t completed_at{};
    };
    std::array<Slot, 3> slots{};
    unsigned slot{};
    ID3D12CommandAllocator* allocator{}; // aliases the current slot
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence* fence{};
    ID3D12Fence* input_fence{};
    HANDLE event{}, shared_handle{}, input_handle{}, output_handle{};
    uint64_t fence_value{}, size{};
    ID3D12Resource* shared{};
    std::array<ID3D12Resource*, 4> textures{};
    std::array<HANDLE, 4> texture_handles{};
    bool Shared(unsigned index) const { return (config.shared_texture_mask & (1u << index)) != 0; }
    std::array<NativeFsr411Plane, 4> planes{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
    std::array<DXGI_FORMAT, 4> formats{};
    std::string error;
    bool ready{}, dispatched{};
    explicit Impl(const NativeFsr411Config& c) : config(c) {}
    ~Impl() {
        try { Wait(fence_value); } catch (const std::exception& e) {
            std::fprintf(stderr, "FSR411 teardown: %s\n", e.what());
        }
        if (context && ffx.DestroyContext) ffx.DestroyContext(&context, nullptr);
        for (auto*& texture : textures) Release(texture);
        for (auto handle : texture_handles)
            if (handle)
                CloseHandle(handle);
        Release(shared);
        if (shared_handle) CloseHandle(shared_handle);
        for (auto& s : slots) { Release(s.list); Release(s.allocator); }
        if (input_handle) CloseHandle(input_handle);
        if (output_handle) CloseHandle(output_handle);
        Release(input_fence); Release(fence); Release(queue);
        Release(device); Release(adapter); Release(factory);
        if (event) CloseHandle(event);
        if (module) FreeLibrary(module);
    }
    ID3D12Resource* Buffer(uint64_t bytes, D3D12_HEAP_TYPE heap_type,
                           D3D12_RESOURCE_STATES state, bool share = false) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heap_type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* result{};
        Check(device->CreateCommittedResource(&heap, share ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
              &desc, state, nullptr, IID_PPV_ARGS(&result)), "Create buffer");
        return result;
    }
    void Wait(uint64_t value) {
        if (!value || !fence) return;
        Check(device->GetDeviceRemovedReason(), "D3D12 device");
        if (fence->GetCompletedValue() >= value) return;
        Check(fence->SetEventOnCompletion(value, event), "Set D3D12 fence event");
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("FSR 4.1.1 D3D12 GPU wait timed out");
        Check(device->GetDeviceRemovedReason(), "D3D12 device");
    }
    void Submit(bool synchronous = true, uint64_t input_value = 0) {
        Check(list->Close(), "Close command list");
        // This queues a GPU dependency and returns immediately to the CPU.
        if (input_value) Check(queue->Wait(input_fence, input_value), "Queue Vulkan input wait");
        ID3D12CommandList* commands[]{list};
        queue->ExecuteCommandLists(1, commands);
        Check(queue->Signal(fence, ++fence_value), "Signal D3D12 fence");
        slots[slot].completed_at = fence_value;
        if (synchronous) Wait(fence_value);
        // FFX and command allocators may only reuse a retired frame slot.
        slot = (slot + 1) % slots.size();
        Wait(slots[slot].completed_at);
        allocator = slots[slot].allocator;
        list = slots[slot].list;
        Check(allocator->Reset(), "Reset allocator");
        Check(list->Reset(allocator, nullptr), "Reset command list");
    }
    void Initialize() {
        const bool driver412 = (config.model_version & 0xfffu) == 2;
        if (!driver412) {
            const auto dll = ModuleDirectory() / L"amd_fidelityfx_upscaler_dx12.dll";
            module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) {
                const auto loader_error = GetLastError();
                std::error_code error;
                if (!std::filesystem::is_regular_file(dll, error))
                    throw std::runtime_error("missing amd_fidelityfx_upscaler_dx12.dll; reinstall the Windows package");
                throw std::runtime_error("cannot load amd_fidelityfx_upscaler_dx12.dll (Windows error " +
                    std::to_string(loader_error) + "); check its runtime dependencies");
            }
            ffxLoadFunctions(&ffx, module);
            if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Query || !ffx.Dispatch)
                throw std::runtime_error("AMD upscaler DLL does not export the FidelityFX API");
        }
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        LUID luid{};
        std::memcpy(&luid, config.adapter_luid, sizeof(luid));
        if (luid.LowPart || luid.HighPart) {
            Check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "Match Vulkan adapter LUID");
        } else {
            Check(factory->EnumAdapters1(0, &adapter), "Enumerate DXGI adapter");
        }
        DXGI_ADAPTER_DESC1 adapter_desc{};
        adapter->GetDesc1(&adapter_desc);
        std::printf("FSR 4.1.1 native: DXGI adapter %ls\n", adapter_desc.Description);
        Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "Create D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Create D3D12 queue");
        for (unsigned i = 0; i < slots.size(); ++i) {
            Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&slots[i].allocator)), "Create command allocator");
            Check(device->CreateCommandList(0, q.Type, slots[i].allocator, nullptr, IID_PPV_ARGS(&slots[i].list)), "Create command list");
            if (i) Check(slots[i].list->Close(), "Close idle command list");
        }
        allocator = slots[0].allocator; list = slots[0].list;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "Create output fence");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&input_fence)), "Create input fence");
        Check(device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &output_handle), "Share output fence");
        Check(device->CreateSharedHandle(input_fence, nullptr, GENERIC_ALL, nullptr, &input_handle), "Share input fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Create fence event failed");
        DriverFsr::External direct{};
        if(driver412){
            direct=DriverFsr::Get(DriverFsr::Runtime(true),FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE);
            if((uint32_t(direct.provider.versionId)&0x7fffffffu)!=config.model_version ||
               !DriverFsr::Supported(direct,device))
                throw std::runtime_error("AMD DX12 4.1.2 provider unavailable on this device");
        }
        uint64_t chosen{};
        if (!driver412) {
            uint64_t count{};
            ffxQueryDescGetVersions versions{};
            versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            versions.device = device;
            versions.outputCount = &count;
            if (ffx.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || count > 64)
                throw std::runtime_error("AMD provider enumeration failed");
            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data(); versions.versionNames = names.data();
            if (ffx.Query(nullptr, &versions.header) != FFX_API_RETURN_OK)
                throw std::runtime_error("AMD provider enumeration failed");
            for (size_t i = 0; i < count; ++i) {
                if (!names[i]) continue;
                std::printf("FSR 4.1.1 native: provider %s\n", names[i]);
                // Require the exact provider instead of accepting the DLL's FSR 3 fallback.
                if (std::strstr(names[i], "4.1.1") &&
                    !std::strstr(names[i], "i8") && !std::strstr(names[i], "Int8")) chosen = ids[i];
            }
            if (!chosen) throw std::runtime_error("AMD FSR4 4.1.1 provider is unavailable on this adapter");
        }
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxOverrideVersion override{};
        override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override.header.pNext = &backend.header;
        override.versionId = chosen;
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version=config.model_version;
        if(driver412){
            ffx.CreateContext=direct.provider.createContext;ffx.DestroyContext=direct.provider.destroyContext;
            ffx.Configure=direct.provider.configure;ffx.Query=direct.provider.query;ffx.Dispatch=direct.provider.dispatch;
            override.header.pNext=nullptr;
            backend.header.pNext=&version.header;
        }
        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = driver412 ? &backend.header : &override.header;
        create.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        if (config.hdr) create.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        else create.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        create.maxRenderSize = {config.render_width, config.render_height};
        create.maxUpscaleSize = {config.output_width, config.output_height};
        const auto rc = ffx.CreateContext(&context, &create.header, nullptr);
        if (rc != FFX_API_RETURN_OK)
            throw std::runtime_error("FSR 4.1.1 context creation returned " + std::to_string(rc));
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (!driver412 && (ffx.Query(&context, &provider.header) != FFX_API_RETURN_OK || !provider.versionName ||
            !std::strstr(provider.versionName, "4.1.1")))
            throw std::runtime_error("AMD context did not select FSR4 4.1.1");
        if(driver412) provider.versionName=direct.provider.versionName;
        std::printf("FSR %s native: ACTIVE %s, %ux%u -> %ux%u, %s\n",driver412?"4.1.2":"4.1.1", provider.versionName,
                    config.render_width, config.render_height, config.output_width, config.output_height,
                    config.hdr ? "HDR" : "sRGB");
        formats = {DXGI_FORMAT(config.color_format), DXGI_FORMAT(config.depth_format),
                   DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (unsigned i = 0; i < textures.size(); ++i) {
            ApiFormat(formats[i]);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = i == 3 ? config.output_width : config.render_width;
            desc.Height = i == 3 ? config.output_height : config.render_height;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format = formats[i]; desc.SampleDesc.Count = 1;
            desc.Flags = i == 3 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            const auto state = Shared(i) ? D3D12_RESOURCE_STATE_COMMON
                                         : (i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                   : D3D12_RESOURCE_STATE_COPY_DEST);
            Check(device->CreateCommittedResource(
                      &heap, Shared(i) ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &desc,
                      state, nullptr, IID_PPV_ARGS(&textures[i])),
                  "Create FSR texture");
            if (Shared(i))
                Check(device->CreateSharedHandle(textures[i], nullptr, GENERIC_ALL, nullptr,
                                                 &texture_handles[i]),
                      "Share D3D12 texture");
            // Shared textures need format metadata, but no range in the copy buffer.
            const uint64_t offset = Shared(i) ? 0 : (size + 511) & ~uint64_t(511);
            UINT rows{}; UINT64 row_bytes{}, bytes{};
            device->GetCopyableFootprints(&desc, 0, 1, offset, &footprints[i], &rows, &row_bytes, &bytes);
            const auto& fp = footprints[i];
            planes[i] = {fp.Offset, fp.Footprint.Width, fp.Footprint.Height,
                         fp.Footprint.RowPitch, uint32_t(row_bytes / desc.Width)};
            if (!Shared(i))
                size = fp.Offset + uint64_t(fp.Footprint.RowPitch) * rows;
        }
        size = std::max(uint64_t(65536), (size + 65535) & ~uint64_t(65535));
        shared = Buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
        Check(device->CreateSharedHandle(shared, nullptr, GENERIC_ALL, nullptr, &shared_handle), "Share D3D12 buffer");
        ready = true;
    }
    FfxApiResource Resource(unsigned index, uint32_t state) {
        FfxApiResource r{};
        r.resource = textures[index]; r.state = state;
        r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        r.description.format = ApiFormat(formats[index]);
        r.description.width = planes[index].width; r.description.height = planes[index].height;
        r.description.depth = 1; r.description.mipCount = 1;
        r.description.usage = index == 3 ? FFX_API_RESOURCE_USAGE_UAV : 0;
        return r;
    }
    void Dispatch(const NativeFsr411Frame& frame, uint64_t input_value = 0) {
        Transition(list, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (unsigned i = 0; i < 3; ++i) {
            if (Shared(i)) {
                Transition(list, textures[i], D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                continue;
            }
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = shared; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[i];
            dst.pResource = textures[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, textures[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (Shared(3))
            Transition(list, textures[3], D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = list;
        d.color = Resource(0, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = Resource(1, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = Resource(2, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = Resource(3, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {frame.jitter_x, frame.jitter_y};
        d.motionVectorScale = {1.0f, 1.0f};
        d.renderSize = {config.render_width, config.render_height};
        d.upscaleSize = {config.output_width, config.output_height};
        d.enableSharpening = frame.sharpen; d.sharpness = frame.sharpness;
        d.frameTimeDelta = frame.frame_ms; d.preExposure = 1.0f;
        d.reset = frame.reset; d.cameraNear = frame.near_plane;
        d.cameraFar = frame.far_plane; d.cameraFovAngleVertical = frame.vertical_fov;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = config.hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const auto rc = ffx.Dispatch(&context, &d.header);
        if (rc != FFX_API_RETURN_OK)
            throw std::runtime_error("FSR 4.1.1 dispatch returned " + std::to_string(rc));
        if (Shared(3)) {
            Transition(list, shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(list, textures[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COMMON);
        } else {
            Transition(list, shared, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            Transition(list, textures[3], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = textures[3];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = shared;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprints[3];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Transition(list, textures[3], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        for (unsigned i = 0; i < 3; ++i)
            Transition(list, textures[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       Shared(i) ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST);
        Submit(!input_value, input_value);
        if (!dispatched) std::printf("FSR 4.1.1 native: first AMD 4.1.1 dispatch submitted (%s)\n", input_value ? "GPU fences" : "CPU waits");
        dispatched = true;
    }
};

bool NativeFsr411::Available() {
    std::error_code error;
    return std::filesystem::is_regular_file(ModuleDirectory() / L"amd_fidelityfx_upscaler_dx12.dll", error);
}
NativeFsr411::NativeFsr411(const NativeFsr411Config& config) : impl(new Impl(config)) {
    try { impl->Initialize(); } catch (const std::exception& e) { impl->error = e.what(); }
}
NativeFsr411::~NativeFsr411() { delete impl; }
bool NativeFsr411::Ready() const { return impl->ready; }
const char* NativeFsr411::Error() const { return impl->error.c_str(); }
void *NativeFsr411::SharedTextureHandle(unsigned index) const {
    return impl->texture_handles.at(index);
}
void* NativeFsr411::SharedHandle() const { return impl->shared_handle; }
uint64_t NativeFsr411::SharedSize() const { return impl->size; }
void* NativeFsr411::InputFenceHandle() const { return impl->input_handle; }
void* NativeFsr411::OutputFenceHandle() const { return impl->output_handle; }
bool NativeFsr411::DispatchAsync(const NativeFsr411Frame& frame, uint64_t input_value, uint64_t& output_value) {
    if (!Ready() || !input_value) return false;
    try { impl->Dispatch(frame, input_value); output_value = impl->fence_value; return true; }
    catch (const std::exception& e) { impl->error = e.what(); impl->ready = false; return false; }
}
const NativeFsr411Plane& NativeFsr411::Plane(unsigned i) const { return impl->planes.at(i); }
bool NativeFsr411::Dispatch(const NativeFsr411Frame& frame) {
    if (!Ready()) return false;
    try { impl->Dispatch(frame); return true; }
    catch (const std::exception& e) { impl->error = e.what(); impl->ready = false; return false; }
}
