// SPDX-FileCopyrightText: Copyright 2026 bbport contributors, bbhost contributors
// SPDX-License-Identifier: MIT
//
// bbport_nr_nvngx.dll: the only part of the project that drives NVIDIA's DLSS 5 Neural Rendering
// model (nvngx_dlssnr.dll, NGX feature 18). Built with MSVC; the port loads it at run time.
//
// This file contains no NVIDIA code and links no NVIDIA runtime: the model is loaded by path at
// Initialize and its entry points are resolved with GetProcAddress. The only thing linked from the
// SDK is the parameter-block allocator, which is the route the SDK's own Vulkan helpers document.
//
// The name is not decoration. The model resolves the module that owns its caller's return address
// and rejects any whose path does not contain "nvngx.dll" with FAIL_PlatformError before reading an
// argument. bbport_nr_nvngx.dll satisfies it; Initialize checks and says so rather than failing
// deep inside the runtime. For the same reason no call into the model is a tail call: a tail call
// becomes a jmp and hands the model whoever called *us* instead of us. Every call is assigned to a
// volatile so the compiler keeps our frame.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include "bbport_nr_bridge.h"

#include <nvsdk_ngx_vk.h>         // NVSDK_NGX_Parameter, NVSDK_NGX_Resource_VK, parameter allocation
#include <nvsdk_ngx_helpers_vk.h> // NVSDK_NGX_Create_ImageView_Resource_VK

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>

namespace {

constexpr uint32_t kFeatureId = 18; // NVSDK_NGX_Feature_NeuralRendering, absent from the public SDK

// Identity the SDK loader is initialised with. The same values the port's upscaling bridge uses, so
// the two bridges announce the port consistently rather than as two unrelated applications.
constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "bbport-windows";

// The parameter block is driven through the SDK's own interface: NVSDK_NGX_Parameter's typed Set and
// Get. The compiler emits the vtable index from the declaration order in nvsdk_ngx_params.h, which is
// the only ordering guaranteed to match the implementation, and every call below is an ordinary C++
// virtual call rather than a slot number chosen by hand.
//
// Picking slots by hand was tried first and is gone. It is not merely fragile: a float written
// through the wrong slot round-trips through a getter of that same wrong type with its bits intact,
// so the probe reports success, the entry ends up with the wrong type, and the model reads a
// denormal where its intensity should be. That failure mode looks like a working bridge.

// The model's Vulkan surface, as the binary exports it. Handles are opaque pointers: every Vulkan
// handle here is dispatchable, and keeping them opaque keeps Vulkan out of these signatures.
//
// The init signature is the SDK's, and was read off the runtime binary rather than taken on trust:
// NVSDK_NGX_VULKAN_Init_Ext reads its sixth argument as a 32-bit value (the version) and its seventh
// as a pointer (the feature info). See the call site for the evidence.
using PFN_NrInit = int(__cdecl*)(unsigned long long, const wchar_t*, void*, void*, void*, int,
                                 const void*);
using PFN_NrCreate = int(__cdecl*)(void*, int, const void*, void**);
using PFN_NrEvaluate = int(__cdecl*)(void*, const void*, const void*, void*);
using PFN_NrRelease = int(__cdecl*)(void*);
using PFN_NrShutdown = int(__cdecl*)(void);
using PFN_NrPopulate = int(__cdecl*)(void*);

struct VkFns {
    PFN_vkCreateEvent create_event = nullptr;
    PFN_vkDestroyEvent destroy_event = nullptr;
    PFN_vkResetEvent reset_event = nullptr;
    PFN_vkGetEventStatus get_event_status = nullptr;
    PFN_vkCmdSetEvent cmd_set_event = nullptr;
};

struct Pass {
    void* feature = nullptr;
    VkEvent ready = VK_NULL_HANDLE;
    BbNrTuning tuning{};
    bool built = false;
    bool signalled = false;
};

struct State {
    BbNrLogFn log = nullptr;
    std::wstring runtime_directory;
    std::wstring data_directory;

    HMODULE snippet = nullptr;
    PFN_NrInit init = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrShutdown snippet_shutdown = nullptr;
    PFN_NrPopulate populate = nullptr;

    NVSDK_NGX_Parameter* parameters = nullptr;

    VkFns vk{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    std::array<Pass, BBPORT_NR_MAX_PASSES> passes{};

    bool configured = false;
    bool initialized = false;
    bool failed = false;
    bool sdk_initialised = false;
};

State g;

void Log(int warning, const char* format, ...) {
    if (g.log == nullptr) {
        return;
    }
    std::array<char, 1024> text{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    g.log(warning, text.data());
}

// ---------------------------------------------------------------------------------------------
// Extension requirements
//
// These are the model's requirements taken from the runtime binary itself, not asked of it: the
// model's own GetFeature*ExtensionRequirements exports are stubs, and the real ones live in the NGX
// core, which does not know about feature 18. The names below are what scanning nvngx_dlssnr.dll
// for VK_*_* yields, and the device names below are the four the model's kernels actually need.
//
// The device list is fixed at vkCreateDevice, so this has to be answered before the caller creates
// one. Asking afterwards is a permanent no, which is why there is no check here: the caller is
// mid-create and there is nothing to check against yet. Initialize verifies what actually arrived.
// ---------------------------------------------------------------------------------------------

const char* const kInstanceExtensionNames[] = {
    "VK_KHR_get_physical_device_properties2",
};

const char* const kDeviceExtensionNames[] = {
    "VK_NVX_binary_import",
    "VK_NVX_image_view_handle",
    "VK_KHR_push_descriptor",
    "VK_KHR_buffer_device_address",
};

std::array<VkExtensionProperties, std::size(kInstanceExtensionNames)> g_instance_extensions{};
std::array<VkExtensionProperties, std::size(kDeviceExtensionNames)> g_device_extensions{};

template <typename Array, typename Names>
void FillExtensionProperties(Array& out, const Names& names) {
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = VkExtensionProperties{};
        out[i].specVersion = 1;
        std::strncpy(out[i].extensionName, names[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
    }
}

// ---------------------------------------------------------------------------------------------
// Parameter block
// ---------------------------------------------------------------------------------------------

void SetUInt(const char* name, unsigned int value) {
    g.parameters->Set(name, value);
}

void SetFloat(const char* name, float value) {
    g.parameters->Set(name, value);
}

// Resources go in as the SDK's own wrapper, through the void* overload: the pattern
// NVSDK_NGX_Create_ImageView_Resource_VK and the SDK's helper structures use.
void SetResource(const char* name, NVSDK_NGX_Resource_VK* value) {
    g.parameters->Set(name, static_cast<void*>(value));
}

// An explicit control mask takes precedence over the runtime's automatic mask. We never supply one,
// so it is cleared every time the toggle is written: the block belongs to the driver and outlives
// the pass, so a stale mask from another feature would otherwise override the setting.
void SetAutomaticMask(int enabled) {
    SetResource("DLSSNR.ControlMask", nullptr);
    SetUInt("DLSSNR.UseAutoMask", enabled != 0 ? 1u : 0u);
}

// One typed round trip, logged, before anything else is written. If the block will not take the
// model's own float parameter then intensity and the three strengths cannot be set, and a bridge that
// runs with controls that do nothing is worse than one that says so.
bool VerifyTypedParameters() {
    constexpr float kProbe = 0.375f; // exact in binary, so the round trip is exact or it is wrong
    static const char* const kKey = "DLSSNR.Intensity";

    g.parameters->Set(kKey, kProbe);

    float read_back = 0.0f;
    const NVSDK_NGX_Result result = g.parameters->Get(kKey, &read_back);
    const bool match = result == NVSDK_NGX_Result_Success && read_back == kProbe;

    Log(0, "Neural Rendering: typed Set/Get round trip on %s: result 0x%08x, value %g%s", kKey,
        static_cast<unsigned>(result), static_cast<double>(read_back), match ? "  MATCH" : "");

    if (!match) {
        Log(1, "Neural Rendering: this block does not accept the model's float parameters, so "
               "intensity and the three strengths cannot be set. Refusing to run with controls that "
               "do nothing.");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------

// The model must be loaded by a module whose path contains "nvngx.dll". Nothing about our own code
// can fix that if the file was renamed, so it is checked once, with a message that says what to do.
bool OurNameSatisfiesCallerGate() {
    HMODULE self = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OurNameSatisfiesCallerGate), &self)) {
        return true; // cannot tell; do not block on our own diagnostic
    }

    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));

    if (length == 0 || length >= path.size()) {
        return true;
    }

    return std::wcsstr(path.data(), L"nvngx.dll") != nullptr;
}

HMODULE LoadSnippet() {
    if (g.snippet != nullptr) {
        return g.snippet;
    }

    // Beside the bridge first, then by bare name so a copy on the search path still works.
    std::wstring beside = g.runtime_directory;

    if (!beside.empty()) {
        if (beside.back() != L'\\' && beside.back() != L'/') {
            beside.push_back(L'\\');
        }
        beside += L"nvngx_dlssnr.dll";

        g.snippet = LoadLibraryExW(beside.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (g.snippet == nullptr) {
        g.snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
    }

    return g.snippet;
}

// ---------------------------------------------------------------------------------------------
// The parameter block
//
// It is the runtime's, not ours, and there are two routes to the same object: the SDK's allocator and
// the driver core's own export. The SDK route refuses with FAIL_FeatureNotSupported when its loader
// has not been through its own initialisation -- which is exactly the state a snippet-driven init
// leaves it in -- so the core's export is the one that works on this path. The core is already in the
// process by then, loaded by the model's own init.
//
// AllocateParameters is asked for before the capability block, on purpose. The capability block is
// documented as the values the runtime already holds, and in a process where the upscaling and
// frame-generation bridge has initialised NGX there is already one of those; taking it would put this
// feature's parameters in a block two threads would then both write. A plain allocation is a private,
// empty block, and the model's own keys are published into it afterwards by PopulateParameters_Impl.
// The capability block is still the fallback for a runtime that will not hand out a plain one.
// ---------------------------------------------------------------------------------------------

using PFN_CoreAllocate = int(__cdecl*)(void**);
using PFN_CoreDestroy = int(__cdecl*)(void*);

HMODULE g_core = nullptr;
PFN_CoreAllocate g_core_allocate = nullptr;
PFN_CoreDestroy g_core_destroy = nullptr;
PFN_CoreAllocate g_core_capabilities = nullptr;
bool g_using_core_allocator = false;

HMODULE OurModule() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OurModule), &self);
    return self;
}

// ---------------------------------------------------------------------------------------------
// Finding the driver core
//
// _nvngx.dll lives in the versioned DriverStore directory of the driver installed *now*. A machine
// that has taken driver updates has several nv_dispi.inf_amd64_* directories, each with its own
// _nvngx.dll, and an old core loads exactly as happily as the current one -- same DLL name, same
// exports -- so "the first one that loads" silently picks whichever the enumeration happens to
// return first. The active directory is therefore asked of the registry first, and the DriverStore
// listing is only the last resort.
// ---------------------------------------------------------------------------------------------

std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY key = nullptr;

    // KEY_WOW64_64KEY: both keys below are written by the 64-bit driver install, and a 32-bit view of
    // them can come back empty on a machine where they exist.
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring result;

    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0 &&
        (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
        std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');

        if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                             &bytes) == ERROR_SUCCESS) {
            // For a multi-string this is the first entry, which is the x64 path; the entries after it
            // are the WoW64 and WSL copies of the same driver.
            result.assign(buffer.c_str());

            if (type == REG_EXPAND_SZ) {
                std::array<wchar_t, 1024> expanded{};

                if (ExpandEnvironmentStringsW(result.c_str(), expanded.data(),
                                              static_cast<DWORD>(expanded.size())) != 0) {
                    result.assign(expanded.data());
                }
            }
        }
    }

    RegCloseKey(key);
    return result;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The directory holding the driver in use right now, or empty when nothing says.
std::wstring ActiveDriverDirectory() {
    // The NGX key names the directory that holds nvngx.dll, _nvngx.dll and the models beside them.
    const std::wstring ngx = ReadRegistryString(HKEY_LOCAL_MACHINE,
                                                L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                                                L"FullPath");

    if (!ngx.empty()) {
        Log(0, "Neural Rendering: the NGX key names the active driver directory: %ls", ngx.c_str());
        return ngx;
    }

    // Otherwise the display class key: one subkey per adapter, each naming its own user-mode driver.
    // A machine can list an AMD adapter and a virtual display here as well, and neither carries
    // _nvngx.dll, so the provider has to match before the path is used.
    const wchar_t* const class_root =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY class_key = nullptr;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_root, 0, KEY_READ | KEY_WOW64_64KEY, &class_key) ==
        ERROR_SUCCESS) {
        std::wstring directory;

        for (DWORD index = 0; directory.empty(); ++index) {
            std::array<wchar_t, 64> name{};
            DWORD length = static_cast<DWORD>(name.size());

            if (RegEnumKeyExW(class_key, index, name.data(), &length, nullptr, nullptr, nullptr,
                              nullptr) != ERROR_SUCCESS) {
                break;
            }

            const std::wstring subkey = std::wstring(class_root) + L"\\" + name.data();

            if (ReadRegistryString(HKEY_LOCAL_MACHINE, subkey.c_str(), L"ProviderName").find(
                    L"NVIDIA") == std::wstring::npos) {
                continue;
            }

            directory = DirectoryOf(
                ReadRegistryString(HKEY_LOCAL_MACHINE, subkey.c_str(), L"UserModeDriverName"));

            if (!directory.empty()) {
                Log(0,
                    "Neural Rendering: the display class key names the active driver directory: %ls",
                    directory.c_str());
            }
        }

        RegCloseKey(class_key);

        if (!directory.empty()) {
            return directory;
        }
    }

    // Registry-free route: the driver's own modules are already mapped in this process, and the
    // directory they were loaded from is the active one by definition.
    for (const wchar_t* module : {L"nvldumdx.dll", L"nvapi64.dll", L"nvoglv64.dll"}) {
        const HMODULE loaded = GetModuleHandleW(module);

        if (loaded == nullptr) {
            continue;
        }

        std::array<wchar_t, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameW(loaded, path.data(), static_cast<DWORD>(path.size()));

        if (length != 0 && length < path.size()) {
            const std::wstring directory = DirectoryOf(path.data());
            Log(0, "Neural Rendering: %ls is mapped from the active driver directory: %ls", module,
                directory.c_str());
            return directory;
        }
    }

    return {};
}

// Tries _nvngx.dll and then nvngx.dll in one directory. The underscore-prefixed name is the NGX core
// itself; the plain one is what an older install carries.
HMODULE LoadCoreBeside(const std::wstring& directory, const wchar_t* description) {
    if (directory.empty()) {
        return nullptr;
    }

    std::wstring base = directory;

    if (base.back() != L'\\' && base.back() != L'/') {
        base.push_back(L'\\');
    }

    for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
        const std::wstring candidate = base + name;
        HMODULE core = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

        if (core != nullptr) {
            Log(0, "Neural Rendering: driver core loaded from the %ls: %ls", description,
                candidate.c_str());
            return core;
        }
    }

    return nullptr;
}

// The last resort, and deliberately not "the first one that loads": the DriverStore keeps every
// driver version ever installed, so the listing is ordered by nothing in particular. The newest
// _nvngx.dll is the best remaining guess, and the log line says a guess is what it was.
HMODULE LoadNewestCoreFromDriverStore() {
    std::array<wchar_t, MAX_PATH> root{};
    const UINT length = GetWindowsDirectoryW(root.data(), static_cast<UINT>(root.size()));

    if (length == 0) {
        return nullptr;
    }

    const std::wstring repository =
        std::wstring(root.data()) + L"\\System32\\DriverStore\\FileRepository";

    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((repository + L"\\nv_dispi*").c_str(), &found);

    if (search == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    std::wstring newest;
    ULARGE_INTEGER newest_time{};

    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }

        const std::wstring candidate = repository + L"\\" + found.cFileName + L"\\_nvngx.dll";
        WIN32_FILE_ATTRIBUTE_DATA attributes{};

        if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &attributes)) {
            continue;
        }

        ULARGE_INTEGER written{};
        written.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        written.HighPart = attributes.ftLastWriteTime.dwHighDateTime;

        if (newest.empty() || written.QuadPart > newest_time.QuadPart) {
            newest = candidate;
            newest_time = written;
        }
    } while (FindNextFileW(search, &found));

    FindClose(search);

    if (newest.empty()) {
        return nullptr;
    }

    Log(0, "Neural Rendering: no active driver directory could be identified; falling back to the "
           "newest core in the DriverStore: %ls",
        newest.c_str());
    return LoadLibraryExW(newest.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool LoadCore() {
    if (g_core != nullptr) {
        return g_core_destroy != nullptr &&
               (g_core_capabilities != nullptr || g_core_allocate != nullptr);
    }

    const HMODULE self = OurModule();

    // If the port's upscaling bridge has already initialised NGX, the core is here under one of these
    // names. It must not be this module: our own name contains "nvngx.dll" too, which is the point of
    // it, and matching ourselves would resolve nothing and look like success.
    for (const wchar_t* name : {L"_nvngx.dll", L"nvngx.dll"}) {
        HMODULE candidate = GetModuleHandleW(name);

        if (candidate != nullptr && candidate != self) {
            g_core = candidate;
            break;
        }
    }

    // If the name lookup misses, ask the loader what is actually mapped. The driver core is installed
    // under a versioned path, and a module loaded from there is matched by base name only.
    if (g_core == nullptr) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);

            if (Module32FirstW(snapshot, &entry)) {
                do {
                    std::wstring base = entry.szModule;
                    std::transform(base.begin(), base.end(), base.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

                    if (entry.hModule == self) {
                        continue;
                    }

                    if (base == L"_nvngx.dll" || base == L"nvngx.dll") {
                        Log(0, "Neural Rendering: mapped NGX module: %ls", entry.szExePath);
                        g_core = entry.hModule;
                    }
                } while (Module32NextW(snapshot, &entry));
            }

            CloseHandle(snapshot);
        }
    }

    if (g_core == nullptr) {
        Log(0, "Neural Rendering: the driver core is not loaded under _nvngx.dll or nvngx.dll; "
               "looking beside the model, then in the active driver directory");

        g_core = LoadCoreBeside(g.runtime_directory, L"model directory");

        if (g_core == nullptr) {
            g_core = LoadLibraryW(L"_nvngx.dll");
        }
        if (g_core == nullptr) {
            g_core = LoadCoreBeside(ActiveDriverDirectory(), L"active driver directory");
        }
        if (g_core == nullptr) {
            g_core = LoadNewestCoreFromDriverStore();
        }
    }

    if (g_core == nullptr) {
        Log(1, "Neural Rendering: the driver core could not be found or loaded");
        return false;
    }

    std::array<wchar_t, MAX_PATH> path{};
    GetModuleFileNameW(g_core, path.data(), static_cast<DWORD>(path.size()));
    Log(0, "Neural Rendering: driver core is %ls", path.data());

    g_core_allocate =
        reinterpret_cast<PFN_CoreAllocate>(GetProcAddress(g_core, "NVSDK_NGX_VULKAN_AllocateParameters"));
    g_core_destroy =
        reinterpret_cast<PFN_CoreDestroy>(GetProcAddress(g_core, "NVSDK_NGX_VULKAN_DestroyParameters"));
    g_core_capabilities = reinterpret_cast<PFN_CoreAllocate>(
        GetProcAddress(g_core, "NVSDK_NGX_VULKAN_GetCapabilityParameters"));

    if (g_core_destroy == nullptr || (g_core_allocate == nullptr && g_core_capabilities == nullptr)) {
        Log(1, "Neural Rendering: the driver core does not export the Vulkan parameter allocator "
               "(allocate %d, capabilities %d, destroy %d)",
            g_core_allocate != nullptr ? 1 : 0, g_core_capabilities != nullptr ? 1 : 0,
            g_core_destroy != nullptr ? 1 : 0);
    }

    return g_core_destroy != nullptr && (g_core_allocate != nullptr || g_core_capabilities != nullptr);
}

// ---------------------------------------------------------------------------------------------
// ABI
// ---------------------------------------------------------------------------------------------

int32_t Configure(const wchar_t* runtime_directory, const wchar_t* data_directory, BbNrLogFn log) {
    g.log = log;
    g.runtime_directory = runtime_directory != nullptr ? runtime_directory : L".";
    g.data_directory = data_directory != nullptr ? data_directory : L".";

    FillExtensionProperties(g_instance_extensions, kInstanceExtensionNames);
    FillExtensionProperties(g_device_extensions, kDeviceExtensionNames);

    g.configured = true;
    return 1;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    if (!g.configured || count == nullptr || extensions == nullptr) {
        return 0;
    }

    *count = static_cast<uint32_t>(g_instance_extensions.size());
    *extensions = g_instance_extensions.data();
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    (void) instance;
    (void) physical;

    if (!g.configured || count == nullptr || extensions == nullptr) {
        return 0;
    }

    *count = static_cast<uint32_t>(g_device_extensions.size());
    *extensions = g_device_extensions.data();
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    Log(0, "Neural Rendering: Initialize entered");

    if (g.failed) {
        return 0;
    }
    if (g.initialized) {
        return 1;
    }
    if (!g.configured || instance == VK_NULL_HANDLE || physical == VK_NULL_HANDLE ||
        device == VK_NULL_HANDLE || get_device_proc == nullptr) {
        Log(1, "Neural Rendering: Initialize called before Configure or without a device");
        return 0;
    }

    if (!OurNameSatisfiesCallerGate()) {
        Log(1, "Neural Rendering: this bridge is not named ...nvngx.dll, and the model refuses "
               "callers whose module path does not contain it. Rename the file back to "
               "bbport_nr_nvngx.dll.");
        g.failed = true;
        return 0;
    }

    Log(0, "Neural Rendering: this module's name satisfies the model's caller gate");

    g.instance = instance;
    g.physical = physical;
    g.device = device;

    g.vk.create_event = reinterpret_cast<PFN_vkCreateEvent>(get_device_proc(device, "vkCreateEvent"));
    g.vk.destroy_event =
        reinterpret_cast<PFN_vkDestroyEvent>(get_device_proc(device, "vkDestroyEvent"));
    g.vk.reset_event = reinterpret_cast<PFN_vkResetEvent>(get_device_proc(device, "vkResetEvent"));
    g.vk.get_event_status =
        reinterpret_cast<PFN_vkGetEventStatus>(get_device_proc(device, "vkGetEventStatus"));
    g.vk.cmd_set_event =
        reinterpret_cast<PFN_vkCmdSetEvent>(get_device_proc(device, "vkCmdSetEvent"));

    if (g.vk.create_event == nullptr || g.vk.destroy_event == nullptr || g.vk.reset_event == nullptr ||
        g.vk.get_event_status == nullptr || g.vk.cmd_set_event == nullptr) {
        Log(1, "Neural Rendering: the device does not expose the event entry points the creation "
               "handshake needs");
        g.failed = true;
        return 0;
    }

    Log(0, "Neural Rendering: device entry points resolved, loading the model");

    if (LoadSnippet() == nullptr) {
        Log(1, "Neural Rendering: nvngx_dlssnr.dll was not found beside the bridge or on the "
               "search path");
        g.failed = true;
        return 0;
    }

    Log(0, "Neural Rendering: nvngx_dlssnr.dll loaded");

    g.init = reinterpret_cast<PFN_NrInit>(GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_Init_Ext"));
    g.create = reinterpret_cast<PFN_NrCreate>(GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_CreateFeature"));
    g.evaluate =
        reinterpret_cast<PFN_NrEvaluate>(GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    g.release =
        reinterpret_cast<PFN_NrRelease>(GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    g.snippet_shutdown =
        reinterpret_cast<PFN_NrShutdown>(GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_Shutdown"));
    g.populate = reinterpret_cast<PFN_NrPopulate>(
        GetProcAddress(g.snippet, "NVSDK_NGX_VULKAN_PopulateParameters_Impl"));

    if (g.init == nullptr || g.create == nullptr || g.evaluate == nullptr || g.release == nullptr) {
        Log(1, "Neural Rendering: nvngx_dlssnr.dll does not expose the native Vulkan entry points");
        g.failed = true;
        return 0;
    }

    Log(0, "Neural Rendering: entry points resolved, initialising the model");

    // Argument order is the SDK's, and it was checked against the runtime binary rather than assumed:
    // NVSDK_NGX_VULKAN_Init_Ext forwards its sixth argument as a 32-bit value into the version
    // parameter and its seventh as a pointer into the feature-info parameter. So the version goes
    // sixth and the feature info seventh; the other way round puts (void*)0x15 in the feature-info
    // slot, which the runtime refuses.
    //
    // Assigned rather than returned: see the note at the top of the file about tail calls.
    volatile int result = g.init(0, g.data_directory.c_str(), instance, physical, device,
                                 NVSDK_NGX_Version_API, nullptr);

    if (result != NVSDK_NGX_Result_Success) {
        Log(1, "Neural Rendering: the model would not initialise on this Vulkan device (0x%08x)",
            static_cast<unsigned>(result));
        g.failed = true;
        return 0;
    }

    Log(0, "Neural Rendering: the model initialised");

    // The SDK's allocator belongs to this DLL's copy of the SDK, and it answers
    // FAIL_OutOfDate until this DLL has been through the SDK's own initialisation. The port's other
    // bridge has its own copy of that state and cannot provide it, so it is done here: one SDK init
    // for this module, then the block.
    {
        NVSDK_NGX_FeatureCommonInfo common{};
        const wchar_t* search_paths[] = {g.runtime_directory.c_str()};
        common.PathListInfo.Path = search_paths;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.LoggingCallback = nullptr;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        common.LoggingInfo.DisableOtherLoggingSinks = true;

        volatile int sdk = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion, g.data_directory.c_str(),
            instance, physical, device, get_instance_proc, get_device_proc, &common,
            NVSDK_NGX_Version_API);

        if (sdk == NVSDK_NGX_Result_Success) {
            g.sdk_initialised = true;
            Log(0, "Neural Rendering: the SDK loader initialised");
        } else {
            Log(0, "Neural Rendering: the SDK loader would not initialise (0x%08x)",
                static_cast<unsigned>(sdk));
        }
    }

    // The plain allocation is asked for first and the capability block is the fallback, which is the
    // other way round from the SDK's sample code: the capability block is shared with whatever else
    // has initialised NGX in this process, and the upscaling and frame-generation bridge has. Two
    // features writing one parameter block from two threads is a race whose symptom is a setting that
    // sometimes does not apply, so the private block is worth preferring.
    NVSDK_NGX_Result allocated = NVSDK_NGX_Result_Fail;

    if (g.sdk_initialised) {
        allocated = NVSDK_NGX_VULKAN_AllocateParameters(&g.parameters);

        if (allocated != NVSDK_NGX_Result_Success || g.parameters == nullptr) {
            g.parameters = nullptr;
            allocated = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g.parameters);
        }
    }

    if (allocated == NVSDK_NGX_Result_Success && g.parameters != nullptr) {
        Log(0, "Neural Rendering: parameter block allocated through the SDK");
    } else {
        Log(0, "Neural Rendering: the SDK allocator refused a parameter block (0x%08x); trying the "
               "driver core",
            static_cast<unsigned>(allocated));

        g.parameters = nullptr;

        const bool have_core = LoadCore();
        int core_result = -1;

        if (have_core) {
            // Same order, and the same reason, as the SDK route above.
            if (g_core_allocate != nullptr) {
                core_result = g_core_allocate(reinterpret_cast<void**>(&g.parameters));
            }

            if ((core_result != NVSDK_NGX_Result_Success || g.parameters == nullptr) &&
                g_core_capabilities != nullptr) {
                g.parameters = nullptr;
                core_result = g_core_capabilities(reinterpret_cast<void**>(&g.parameters));
            }
        }

        if (!have_core || core_result != NVSDK_NGX_Result_Success || g.parameters == nullptr) {
            Log(1, "Neural Rendering: no parameter block could be obtained from the SDK or the "
                   "driver core (core result 0x%08x)",
                static_cast<unsigned>(core_result));
            g.failed = true;
            return 0;
        }

        g_using_core_allocator = true;
        Log(0, "Neural Rendering: parameter block allocated through the driver core");
    }

    // The block is the core's; the DLSSNR.* parameters are the model's. Publishing them is what puts
    // the model's keys into the block, and without it every DLSSNR.* read answers
    // FAIL_UnsupportedParameter (0xBAD00010) -- which is also what made the float search look like
    // "no float slot exists" when the real answer was "no such parameter, yet".
    if (g.populate != nullptr) {
        volatile int published = g.populate(g.parameters);
        Log(0, "Neural Rendering: the model published its parameters (0x%08x)",
            static_cast<unsigned>(published));
    } else {
        Log(1, "Neural Rendering: the model does not export PopulateParameters_Impl; its parameters "
               "may not be visible to the block");
    }

    if (!VerifyTypedParameters()) {
        g.failed = true;
        return 0;
    }

    g.initialized = true;
    Log(0, "Neural Rendering: the model initialised on this device");
    return 1;
}

int32_t CreatePass(VkCommandBuffer command, uint32_t index, const BbNrTuning* tuning) {
    if (!g.initialized || command == VK_NULL_HANDLE || tuning == nullptr ||
        index >= BBPORT_NR_MAX_PASSES || tuning->width == 0 || tuning->height == 0) {
        return 0;
    }

    Pass& pass = g.passes[index];

    // Rebuilding is allowed, and requires that the caller has drained the GPU: the feature and the
    // event about to be replaced may still be referenced by submitted work.
    if (pass.feature != nullptr) {
        g.release(pass.feature);
        pass.feature = nullptr;
    }
    pass.built = false;
    pass.signalled = false;

    SetUInt("DLSSNR.Enabled", 1);
    SetUInt("DLSSNR.Width", tuning->width);
    SetUInt("DLSSNR.Height", tuning->height);
    SetUInt("CreationNodeMask", 1);
    SetUInt("VisibilityNodeMask", 1);

    // Written unconditionally, including zero: the block outlives the pass, so skipping a write for
    // "default" leaves the previously chosen value sitting in it.
    SetUInt("DLSSNR.Hint.Render.Preset", tuning->preset);
    SetFloat("DLSSNR.Intensity", tuning->intensity);
    SetUInt("DLSSNR.Style", tuning->style);
    SetFloat("DLSSNR.LocalStructureStrength", tuning->structure);
    SetFloat("DLSSNR.LocalToneStrength", tuning->tone);
    SetFloat("DLSSNR.SkinStructureStrength", tuning->skin_structure);
    SetAutomaticMask(tuning->auto_skin != 0 ? 1 : 0);
    SetUInt("DLSSNR.UICorrection", tuning->ui_correction);

    void* feature = nullptr;
    volatile int result = g.create(command, static_cast<int>(kFeatureId), g.parameters, &feature);

    if (result != NVSDK_NGX_Result_Success || feature == nullptr) {
        Log(1, "Neural Rendering: pass %u would not build at %ux%u (0x%08x)", index, tuning->width,
            tuning->height, static_cast<unsigned>(result));
        return 0;
    }

    if (pass.ready == VK_NULL_HANDLE) {
        VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};

        if (g.vk.create_event(g.device, &info, nullptr, &pass.ready) != VK_SUCCESS) {
            Log(1, "Neural Rendering: pass %u could not allocate its creation marker", index);
            g.release(feature);
            return 0;
        }
    } else {
        // The caller drained the GPU before rebuilding, so nothing is still watching this event.
        g.vk.reset_event(g.device, pass.ready);
    }

    // Creation records GPU uploads into this command buffer, so the pass is built but not usable
    // until that submission has finished. The marker is how ReadyPass knows.
    g.vk.cmd_set_event(command, pass.ready, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    pass.feature = feature;
    pass.tuning = *tuning;
    pass.built = true;
    pass.signalled = false;

    Log(0, "Neural Rendering: pass %u built at %ux%u", index, tuning->width, tuning->height);
    return 1;
}

int32_t ReadyPass(uint32_t index) {
    if (!g.initialized || index >= BBPORT_NR_MAX_PASSES) {
        return 0;
    }

    Pass& pass = g.passes[index];

    if (!pass.built) {
        return 0;
    }
    if (pass.signalled) {
        return 1;
    }

    if (g.vk.get_event_status(g.device, pass.ready) == VK_EVENT_SET) {
        pass.signalled = true;
        return 1;
    }

    return 0;
}

bool FormatMatches(const BbNrImage& image, VkFormat expected) {
    return image.view != VK_NULL_HANDLE && image.image != VK_NULL_HANDLE && image.format == expected;
}

int32_t EvaluatePass(VkCommandBuffer command, uint32_t index, const BbNrEvaluate* evaluate) {
    if (!g.initialized || index >= BBPORT_NR_MAX_PASSES || command == VK_NULL_HANDLE ||
        evaluate == nullptr) {
        return 0;
    }

    Pass& pass = g.passes[index];

    // A pass that is built but whose creation submission has not retired cannot be evaluated. The
    // caller is expected to have polled ReadyPass; this is the backstop, not the mechanism.
    if (!pass.built || !pass.signalled) {
        return 0;
    }

    if (!FormatMatches(evaluate->color, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !FormatMatches(evaluate->output, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !FormatMatches(evaluate->motion, VK_FORMAT_R16G16_SFLOAT) ||
        !FormatMatches(evaluate->depth, VK_FORMAT_R32_SFLOAT)) {
        Log(1, "Neural Rendering: pass %u was given the wrong formats. Colour and output must be "
               "RGBA16_SFLOAT, motion RG16_SFLOAT and depth R32_SFLOAT.",
            index);
        return 0;
    }

    const uint32_t width = pass.tuning.width;
    const uint32_t height = pass.tuning.height;

    NVSDK_NGX_Resource_VK color_resource =
        NVSDK_NGX_Create_ImageView_Resource_VK(evaluate->color.view, evaluate->color.image,
                                               evaluate->color.range, evaluate->color.format,
                                               evaluate->color.width, evaluate->color.height, false);
    NVSDK_NGX_Resource_VK motion_resource =
        NVSDK_NGX_Create_ImageView_Resource_VK(evaluate->motion.view, evaluate->motion.image,
                                               evaluate->motion.range, evaluate->motion.format,
                                               evaluate->motion.width, evaluate->motion.height, false);
    NVSDK_NGX_Resource_VK depth_resource =
        NVSDK_NGX_Create_ImageView_Resource_VK(evaluate->depth.view, evaluate->depth.image,
                                               evaluate->depth.range, evaluate->depth.format,
                                               evaluate->depth.width, evaluate->depth.height, false);
    NVSDK_NGX_Resource_VK output_resource =
        NVSDK_NGX_Create_ImageView_Resource_VK(evaluate->output.view, evaluate->output.image,
                                               evaluate->output.range, evaluate->output.format,
                                               evaluate->output.width, evaluate->output.height, true);

    SetResource("DLSSNR.Color", &color_resource);
    SetResource("DLSSNR.Depth", &depth_resource);
    SetResource("DLSSNR.MVec", &motion_resource);
    SetResource("DLSSNR.Output", &output_resource);

    // The block is shared with whatever else drives the runtime, so every value the feature reads is
    // written here rather than relied on from creation.
    SetUInt("DLSSNR.Enabled", 1);
    SetUInt("DLSSNR.Width", width);
    SetUInt("DLSSNR.Height", height);
    SetUInt("DLSSNR.DepthInverted", evaluate->depth_inverted);
    SetUInt("DLSSNR.Reset", evaluate->reset);

    SetUInt("DLSSNR.ColorSubrectBaseX", 0);
    SetUInt("DLSSNR.ColorSubrectBaseY", 0);
    SetUInt("DLSSNR.ColorSubrectWidth", evaluate->color.width);
    SetUInt("DLSSNR.ColorSubrectHeight", evaluate->color.height);
    SetUInt("DLSSNR.OutputSubrectBaseX", 0);
    SetUInt("DLSSNR.OutputSubrectBaseY", 0);
    SetUInt("DLSSNR.OutputSubrectWidth", evaluate->output.width);
    SetUInt("DLSSNR.OutputSubrectHeight", evaluate->output.height);
    SetUInt("DLSSNR.DepthSubrectBaseX", 0);
    SetUInt("DLSSNR.DepthSubrectBaseY", 0);
    SetUInt("DLSSNR.DepthSubrectWidth", evaluate->depth.width);
    SetUInt("DLSSNR.DepthSubrectHeight", evaluate->depth.height);
    SetUInt("DLSSNR.MVecSubrectBaseX", 0);
    SetUInt("DLSSNR.MVecSubrectBaseY", 0);
    SetUInt("DLSSNR.MVecSubrectWidth", evaluate->motion.width);
    SetUInt("DLSSNR.MVecSubrectHeight", evaluate->motion.height);

    // The game's own encoding, passed through rather than derived: deriving it from the resolutions
    // comes out as exactly 1.0 at native, which tells the model almost nothing moved.
    SetFloat("DLSSNR.MVecScaleX", evaluate->mv_scale_x);
    SetFloat("DLSSNR.MVecScaleY", evaluate->mv_scale_y);

    SetFloat("DLSSNR.Intensity", pass.tuning.intensity);
    SetUInt("DLSSNR.Style", pass.tuning.style);
    SetFloat("DLSSNR.LocalStructureStrength", pass.tuning.structure);
    SetFloat("DLSSNR.LocalToneStrength", pass.tuning.tone);
    SetFloat("DLSSNR.SkinStructureStrength", pass.tuning.skin_structure);
    SetAutomaticMask(pass.tuning.auto_skin != 0 ? 1 : 0);

    volatile int result = g.evaluate(command, pass.feature, g.parameters, nullptr);

    if (result != NVSDK_NGX_Result_Success) {
        Log(1, "Neural Rendering: pass %u refused to evaluate (0x%08x)", index,
            static_cast<unsigned>(result));
        return 0;
    }

    return 1;
}

void ReleasePass(uint32_t index) {
    if (index >= BBPORT_NR_MAX_PASSES) {
        return;
    }

    Pass& pass = g.passes[index];

    if (pass.feature != nullptr && g.release != nullptr) {
        volatile int ignored = g.release(pass.feature);
        (void) ignored;
    }
    pass.feature = nullptr;

    if (pass.ready != VK_NULL_HANDLE && g.vk.destroy_event != nullptr) {
        g.vk.destroy_event(g.device, pass.ready, nullptr);
    }
    pass.ready = VK_NULL_HANDLE;
    pass.built = false;
    pass.signalled = false;
}

void Shutdown() {
    for (uint32_t index = 0; index < BBPORT_NR_MAX_PASSES; ++index) {
        ReleasePass(index);
    }

    if (g.parameters != nullptr) {
        if (g_using_core_allocator && g_core_destroy != nullptr) {
            g_core_destroy(g.parameters);
        } else {
            NVSDK_NGX_VULKAN_DestroyParameters(g.parameters);
        }
        g.parameters = nullptr;
        g_using_core_allocator = false;
    }

    // The module stays loaded on purpose: unloading the runtime while the driver still holds state
    // for it is a documented way to lose the device, and there is nothing to gain here.
    if (g.sdk_initialised && g.device != VK_NULL_HANDLE) {
        volatile int ignored = NVSDK_NGX_VULKAN_Shutdown1(g.device);
        (void) ignored;
        g.sdk_initialised = false;
    }

    // The model's own teardown, called before the device goes away. Leaving the runtime holding
    // device state into process exit is what turns a clean quit into a crash in its DLL detach.
    if (g.snippet_shutdown != nullptr) {
        volatile int ignored = g.snippet_shutdown();
        (void) ignored;
    }

    g.init = nullptr;
    g.create = nullptr;
    g.evaluate = nullptr;
    g.release = nullptr;
    g.snippet_shutdown = nullptr;
    g.populate = nullptr;

    g.instance = VK_NULL_HANDLE;
    g.physical = VK_NULL_HANDLE;
    g.device = VK_NULL_HANDLE;
    g.vk = VkFns{};

    g.initialized = false;
    g.failed = false;
}

const BbNrApi api{
    BBPORT_NR_BRIDGE_ABI,
    sizeof(BbNrApi),
    Configure,
    InstanceExtensions,
    DeviceExtensions,
    Initialize,
    CreatePass,
    ReadyPass,
    EvaluatePass,
    ReleasePass,
    Shutdown,
};

} // namespace

extern "C" __declspec(dllexport) const BbNrApi* BbNrGetApi() {
    return &api;
}
