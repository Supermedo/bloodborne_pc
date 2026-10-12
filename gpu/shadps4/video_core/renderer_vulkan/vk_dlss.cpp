// SPDX-FileCopyrightText: Copyright 2026 IFreemz, bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_dlss.h"

#if defined(_WIN32) || defined(__linux__)
#define BB_DLSS_BRIDGE 1
#include "../../../dlss_bridge/bbport_dlss_bridge.h"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace Vulkan {

int Dlss::QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#ifdef BB_DLSS_BRIDGE

namespace {
#ifdef _WIN32
constexpr wchar_t BridgeName[] = L"bbport_dlss.dll";
using Module = HMODULE;

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

bool HasNgxLibrary(const std::filesystem::path& directory) {
    return std::filesystem::is_regular_file(directory / L"nvngx_dlss.dll");
}

Module LoadBridge(const std::filesystem::path& path) {
    return LoadLibraryW(path.c_str());
}

void* BridgeSymbol(Module module, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}
#else
constexpr char BridgeName[] = "libbbport_dlss.so";
using Module = void*;

std::filesystem::path ExecutableDirectory() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::current_path() : path.parent_path();
}

// NGX looks for libnvidia-ngx-dlss.so.<version> in the directory passed to Configure.
bool HasNgxLibrary(const std::filesystem::path& directory) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().starts_with("libnvidia-ngx-dlss.so.")) {
            return true;
        }
    }
    return false;
}

Module LoadBridge(const std::filesystem::path& path) {
    Module module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::printf("DLSS: %s\n", dlerror());
    }
    return module;
}

void* BridgeSymbol(Module module, const char* name) {
    return dlsym(module, name);
}
#endif

void BridgeLog(int warning, const char* message) {
    std::printf("DLSS: %s%s\n", warning ? "warning: " : "", message);
}

bool CollectExtensions(const VkExtensionProperties* required, u32 count,
                       const std::vector<vk::ExtensionProperties>& available,
                       std::vector<std::string>& storage, std::vector<const char*>& enabled) {
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("DLSS: missing Vulkan extension %s\n", request.extensionName);
            return false;
        }
        storage.emplace_back(request.extensionName);
    }
    for (const auto& name : storage) {
        if (std::ranges::none_of(enabled, [&](const char* current) { return name == current; })) {
            enabled.push_back(name.c_str());
        }
    }
    return true;
}
} // namespace

struct Dlss::Impl {
    Module module{};
    const BbDlssApi* api{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("DLSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
};

Dlss* Dlss::Get() {
    static Dlss* const dlss = []() -> Dlss* {
        const char* setting = std::getenv("BB_DLSS");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        const auto directory = ExecutableDirectory();
        if (!std::filesystem::is_regular_file(directory / BridgeName) ||
            !HasNgxLibrary(directory)) {
            return nullptr;
        }
        auto* created = new Dlss;
        // The process may end through quick_exit/_exit paths: release NGX first when it can.
        std::atexit([] {
            if (Dlss* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return dlss;
}

Dlss::Dlss() : impl{std::make_unique<Impl>()} {
    const auto directory = ExecutableDirectory();
    impl->module = LoadBridge(directory / BridgeName);
    const auto get_api =
        impl->module ? reinterpret_cast<BbDlssGetApiFn>(BridgeSymbol(impl->module, "BbDlssGetApi"))
                     : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    if (!impl->api || impl->api->abi != BBPORT_DLSS_BRIDGE_ABI) {
        impl->Disable("the DLSS bridge library is missing or from another version");
        return;
    }
    // NGX writes its logs and model updates here: beside the saves and shader caches.
    std::filesystem::path data = directory / "dlss";
    if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && user[0]) {
        data = std::filesystem::path{user} / "dlss";
    }
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (!impl->api->Configure(directory.wstring().c_str(), data.wstring().c_str(), BridgeLog)) {
        impl->Disable("bridge configuration failed");
    }
}

Dlss::~Dlss() {
    Shutdown();
}

void Dlss::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->InstanceExtensions(&count, &required)) {
        return impl->Disable("instance extension query failed");
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->instance_extensions, enabled)) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    impl->instance_ready = true;
}

void Dlss::AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready) {
        return;
    }
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        return impl->Disable("not an NVIDIA GPU");
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->DeviceExtensions(instance, physical, &count, &required)) {
        return impl->Disable("this GPU or driver does not support DLSS (GeForce RTX needed)");
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->device_extensions, enabled)) {
        return impl->Disable("required Vulkan device extensions are missing");
    }
    impl->device_ready = true;
}

void Dlss::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    if (!impl->api->Initialize(instance, physical, device,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("NVIDIA NGX initialization failed (update the GPU driver)");
    }
    impl->available = true;
    std::printf("DLSS: ready\n");
}

bool Dlss::Available() const {
    return impl->available;
}

std::string Dlss::Problem() const {
    return impl->problem;
}

bool Dlss::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Dlss::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    const BbDlssFeature feature{desc.input_width,  desc.input_height, desc.output_width,
                                desc.output_height, desc.quality,      0,
                                desc.preset,        desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool Dlss::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!impl->available || !impl->feature) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return BbDlssImage{r.image,
                           r.view,
                           static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format),
                           r.width,
                           r.height};
    };
    const BbDlssEvaluate parameters{image(frame.color),  image(frame.depth), image(frame.motion),
                                    image(frame.output), frame.jitter_x,     frame.jitter_y,
                                    frame.reset ? 1 : 0, frame.frame_ms,     frame.sharpness};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void Dlss::ReleaseFeature() {
    impl->feature.reset();
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

void Dlss::Shutdown() {
    impl->available = false;
    impl->feature.reset();
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
}

#else // no bridge on this platform

struct Dlss::Impl {};
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
Dlss* Dlss::Get() {
    return nullptr;
}
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const {
    return false;
}
std::string Dlss::Problem() const {
    return {};
}
bool Dlss::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Dlss::ReleaseFeature() {}

#endif

} // namespace Vulkan
