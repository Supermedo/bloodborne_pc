// SPDX-License-Identifier: GPL-2.0-or-later
#include "remix_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d9.h>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

namespace BbRemix {
namespace {
void Debug(const char *text) {
  if (std::getenv("BB_REMIX_DEBUG")) {
    std::printf("Remix phase: %s\n", text);
    std::fflush(stdout);
  }
}
bool Dimensions(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= 8192 && height <= 8192;
}
template <class T> void Release(T *&value) {
  if (value)
    value->Release();
  value = nullptr;
}
bool Finite(const float *values, size_t count) {
  return std::all_of(values, values + count,
                     [](float v) { return std::isfinite(v); });
}
} // namespace

struct Renderer::Impl {
  remixapi_Interface api{};
  HMODULE module{};
  DLL_DIRECTORY_COOKIE dll_directory{};
  HWND window{};
  IDirect3D9Ex *d3d{};
  IDirect3DDevice9Ex *device{};
  IDirect3DTexture9 *output_texture{};
  IDirect3DSurface9 *output_surface{};
  IDirect3DSurface9 *readback{};
  IDirect3DQuery9 *done{};
  HANDLE shared{};
  std::array<uint8_t, 8> adapter_luid{};
  uint32_t width{}, height{};
  uint64_t generation{};
  bool registered{}, pending{}, rendered{}, failed{}, camera_ready{};
  std::string error;

  template <class T> struct Resource {
    T handle{};
    uint64_t revision{};
  };
  struct MeshResource : Resource<remixapi_MeshHandle> {
    std::vector<remixapi_MaterialHandle> materials;
  };
  std::unordered_map<uint64_t, Resource<remixapi_MaterialHandle>> materials;
  std::unordered_map<uint64_t, MeshResource> meshes;
  std::unordered_map<uint64_t, Resource<remixapi_LightHandle>> lights;

  bool Fail(const char *operation, long long status, bool fatal = false) {
    error = std::string(operation) + " failed (" + std::to_string(status) + ")";
    failed |= fatal;
    std::printf("RTX Remix SDK: %s\n", error.c_str());
    return false;
  }
  bool Check(remixapi_ErrorCode status, const char *operation) {
    return status == REMIXAPI_ERROR_CODE_SUCCESS ||
           Fail(operation, status, true);
  }
  bool CheckHr(HRESULT status, const char *operation) {
    return SUCCEEDED(status) || Fail(operation, uint32_t(status), true);
  }
  bool Usable() const { return registered && !failed; }
  void ReleaseOutput() {
    Release(readback);
    Release(output_surface);
    Release(output_texture);
    shared = nullptr;
    rendered = false;
  }
};

Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() { Shutdown(); }
const std::string &Renderer::Error() const { return impl->error; }
remixapi_MaterialInfoOpaqueEXT Renderer::OpaqueDefaults() {
  remixapi_MaterialInfoOpaqueEXT info{};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  info.albedoConstant = {1, 1, 1};
  info.opacityConstant = 1;
  info.roughnessConstant = 0.5f;
  info.alphaTestType = 7; // SDK AlphaTestType::kAlways == VK_COMPARE_OP_ALWAYS
  return info;
}
remixapi_MaterialInfo Renderer::MaterialDefaults(uint64_t id) {
  remixapi_MaterialInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.hash = id;
  info.spriteSheetRow = info.spriteSheetCol = 1;
  return info;
}

bool Renderer::Initialize(const std::filesystem::path &path, uint32_t width,
                          uint32_t height, std::array<uint8_t, 8> adapter) {
  auto &p = *impl;
  if (p.module || !Dimensions(width, height) || !path.is_absolute())
    return p.Fail("Initialize arguments", -1);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return p.Fail("runtime DLL missing", -1);
  p.failed = false;
  // Absolute DLL path and retained dependency directory: no global
  // SetDllDirectory or changing the game's working directory. Delayed
  // USD/NVIDIA loads need this too.
  p.dll_directory = AddDllDirectory(path.parent_path().c_str());
  if (!p.dll_directory)
    return p.Fail("AddDllDirectory", GetLastError());
  p.module = LoadLibraryExW(path.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!p.module)
    return p.Fail("LoadLibraryExW", GetLastError());
  auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(
      GetProcAddress(p.module, "remixapi_InitializeLibrary"));
  if (!initialize)
    return p.Fail("remixapi_InitializeLibrary export", GetLastError());
  const remixapi_InitializeLibraryInfo info{
      REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO, nullptr,
      REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR,
                            REMIXAPI_VERSION_PATCH)};
  if (!p.Check(initialize(&info, &p.api), "InitializeLibrary"))
    return false;
  if (!p.api.dxvk_CreateD3D9 || !p.api.dxvk_RegisterD3D9Device ||
      !p.api.Present || !p.api.dxvk_CopyRenderingOutput || !p.api.CreateMesh ||
      !p.api.CreateMaterial || !p.api.CreateLight || !p.api.Shutdown ||
      !p.api.SetupCamera || !p.api.DrawInstance || !p.api.DrawLightInstance ||
      !p.api.DestroyMesh || !p.api.DestroyMaterial || !p.api.DestroyLight ||
      !p.api.SetConfigVariable)
    return p.Fail("incomplete SDK interface", -1, true);

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"BbRemixSdkOutput";
  if (!RegisterClassW(&window_class) &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return p.Fail("RegisterClassW", GetLastError(), true);
  p.window = CreateWindowExW(0, window_class.lpszClassName,
                             L"Bloodborne RTX Remix SDK output", WS_POPUP, 0, 0,
                             int(width), int(height), nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
  if (!p.window)
    return p.Fail("CreateWindowExW", GetLastError(), true);
  // Use the documented D3D9 interop registration path. forceNoVkSwapchain's raw
  // semaphore handles are device-local and cannot be waited on by our Vulkan
  // device.
  if (!p.Check(p.api.dxvk_CreateD3D9(false, &p.d3d), "CreateD3D9"))
    return false;
  LUID luid{};
  UINT adapter_index = D3DADAPTER_DEFAULT;
  if (std::any_of(adapter.begin(), adapter.end(), [](uint8_t b) { return b != 0; })) {
    bool found = false;
    for (UINT i = 0; i < p.d3d->GetAdapterCount(); ++i) {
      LUID candidate{};
      if (SUCCEEDED(p.d3d->GetAdapterLUID(i, &candidate)) &&
          !std::memcmp(&candidate, adapter.data(), sizeof(candidate))) {
        adapter_index = i;
        found = true;
        break;
      }
    }
    if (!found) return p.Fail("game Vulkan adapter unavailable in Remix", -1, true);
  }
  if (!p.CheckHr(p.d3d->GetAdapterLUID(adapter_index, &luid),
                 "GetAdapterLUID"))
    return false;
  std::memcpy(p.adapter_luid.data(), &luid, sizeof(luid));
  D3DPRESENT_PARAMETERS present{};
  present.BackBufferWidth = width;
  present.BackBufferHeight = height;
  present.BackBufferFormat = D3DFMT_A8R8G8B8;
  present.BackBufferCount = 1;
  present.SwapEffect = D3DSWAPEFFECT_DISCARD;
  present.hDeviceWindow = p.window;
  present.Windowed = TRUE;
  present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  if (!p.CheckHr(p.d3d->CreateDeviceEx(adapter_index, D3DDEVTYPE_HAL,
                                       p.window,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                       &present, nullptr, &p.device),
                 "CreateDeviceEx"))
    return false;
  if (!p.Check(p.api.dxvk_RegisterD3D9Device(p.device), "RegisterD3D9Device"))
    return false;
  p.registered = true;
  if (!p.CheckHr(p.device->CreateQuery(D3DQUERYTYPE_EVENT, &p.done),
                 "CreateQuery"))
    return false;
  // The final output is already tonemapped. Keep its floating-point
  // representation; composing it into an HDR scene before the game's tonemap
  // would tonemap it twice.
  if (!Resize(width, height))
    return false;
  std::printf("RTX Remix SDK: initialized official API %d.%d.%d (%ux%u)\n",
              REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR,
              REMIXAPI_VERSION_PATCH, width, height);
  return true;
}

bool Renderer::Resize(uint32_t width, uint32_t height) {
  auto &p = *impl;
  if (!p.Usable() || !Dimensions(width, height))
    return p.Fail("Resize arguments", -1);
  if (p.output_texture && p.width == width && p.height == height)
    return true;
  if (!Wait())
    return false;
  p.ReleaseOutput();
  IDirect3DSwapChain9 *swapchain{};
  D3DPRESENT_PARAMETERS present{};
  if (!p.CheckHr(p.device->GetSwapChain(0, &swapchain), "GetSwapChain"))
    return false;
  const HRESULT hr = swapchain->GetPresentParameters(&present);
  Release(swapchain);
  if (!p.CheckHr(hr, "GetPresentParameters"))
    return false;
  present.BackBufferWidth = width;
  present.BackBufferHeight = height;
  SetWindowPos(p.window, nullptr, 0, 0, int(width), int(height),
               SWP_NOACTIVATE | SWP_NOZORDER);
  // Explicit reset handles one-axis resizes too (SDK Present checks both
  // dimensions).
  if (!p.CheckHr(p.device->ResetEx(&present, nullptr), "ResetEx"))
    return false;
  p.shared = nullptr;
  if (!p.CheckHr(p.device->CreateTexture(width, height, 1,
                                         D3DUSAGE_RENDERTARGET,
                                         D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                                         &p.output_texture, &p.shared),
                 "CreateTexture shared output"))
    return false;
  if (!p.shared)
    return p.Fail("shared texture export", -1, true);
  if (!p.CheckHr(p.output_texture->GetSurfaceLevel(0, &p.output_surface),
                 "GetSurfaceLevel"))
    return false;
  p.width = width;
  p.height = height;
  ++p.generation;
  return true;
}

remixapi_MaterialHandle Renderer::MaterialHandle(uint64_t id) const {
  const auto it = impl->materials.find(id);
  return it == impl->materials.end() ? nullptr : it->second.handle;
}

bool Renderer::Material(const remixapi_MaterialInfo &info, uint64_t revision) {
  auto &p = *impl;
  if (!p.Usable() || !info.hash ||
      info.sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO)
    return p.Fail("Material arguments", -1);
  if (const auto it = p.materials.find(info.hash); it != p.materials.end()) {
    if (it->second.revision == revision)
      return true;
    if (!DestroyMaterial(info.hash))
      return false;
  }
  remixapi_MaterialHandle handle{};
  if (!p.Check(p.api.CreateMaterial(&info, &handle), "CreateMaterial"))
    return false;
  p.materials.emplace(
      info.hash, Impl::Resource<remixapi_MaterialHandle>{handle, revision});
  return true;
}

bool Renderer::Mesh(const remixapi_MeshInfo &info, uint64_t revision) {
  auto &p = *impl;
  if (!p.Usable() || !info.hash ||
      info.sType != REMIXAPI_STRUCT_TYPE_MESH_INFO || !info.surfaces_values ||
      !info.surfaces_count || info.surfaces_count > 4096)
    return p.Fail("Mesh arguments", -1);
  if (const auto it = p.meshes.find(info.hash);
      it != p.meshes.end() && it->second.revision == revision)
    return true;
  Impl::MeshResource resource;
  for (uint32_t s = 0; s < info.surfaces_count; ++s) {
    const auto &surface = info.surfaces_values[s];
    if (!surface.vertices_values || !surface.vertices_count ||
        surface.vertices_count > (16u << 20) ||
        surface.indices_count > (64u << 20) ||
        (surface.indices_count &&
         (!surface.indices_values || surface.indices_count % 3)) ||
        (!surface.indices_count && surface.vertices_count % 3))
      return p.Fail("Mesh triangle data", -1);
    for (uint64_t i = 0; i < surface.vertices_count; ++i) {
      const auto &vertex = surface.vertices_values[i];
      if (!Finite(vertex.position, 3) || !Finite(vertex.normal, 3) ||
          !Finite(vertex.texcoord, 2))
        return p.Fail("non-finite mesh vertex", -1);
    }
    for (uint64_t i = 0; i < surface.indices_count; ++i)
      if (surface.indices_values[i] >= surface.vertices_count)
        return p.Fail("mesh index outside vertex buffer", -1);
    if (surface.skinning_hasvalue) {
      const auto &skin = surface.skinning_value;
      const uint64_t n = surface.vertices_count * skin.bonesPerVertex;
      if (!skin.bonesPerVertex || skin.bonesPerVertex > 4 ||
          !skin.blendWeights_values || !skin.blendIndices_values ||
          n != skin.blendWeights_count || n != skin.blendIndices_count ||
          !Finite(skin.blendWeights_values, size_t(n)))
        return p.Fail("skinning data", -1);
      for (uint64_t i = 0; i < n; ++i)
        if (skin.blendIndices_values[i] >=
                REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT ||
            skin.blendWeights_values[i] < 0)
          return p.Fail("skinning bone/weight", -1);
    }
    if (surface.material) {
      if (std::none_of(p.materials.begin(), p.materials.end(),
                       [&](const auto &entry) {
                         return entry.second.handle == surface.material;
                       }))
        return p.Fail("unregistered mesh material", -1);
      resource.materials.push_back(surface.material);
    }
  }
  if (!DestroyMesh(info.hash))
    return false;
  if (!p.Check(p.api.CreateMesh(&info, &resource.handle), "CreateMesh"))
    return false;
  resource.revision = revision;
  p.meshes.emplace(info.hash, std::move(resource));
  return true;
}

bool Renderer::Light(const remixapi_LightInfo &info, uint64_t revision) {
  auto &p = *impl;
  if (!p.Usable() || !info.hash ||
      info.sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO)
    return p.Fail("Light arguments", -1);
  if (const auto it = p.lights.find(info.hash); it != p.lights.end()) {
    if (it->second.revision == revision)
      return true;
    if (!DestroyLight(info.hash))
      return false;
  }
  remixapi_LightHandle handle{};
  if (!p.Check(p.api.CreateLight(&info, &handle), "CreateLight"))
    return false;
  p.lights.emplace(info.hash,
                   Impl::Resource<remixapi_LightHandle>{handle, revision});
  return true;
}

bool Renderer::DestroyMesh(uint64_t id) {
  auto &p = *impl;
  const auto it = p.meshes.find(id);
  if (it == p.meshes.end())
    return true;
  if (!p.Usable() || !Wait() ||
      !p.Check(p.api.DestroyMesh(it->second.handle), "DestroyMesh"))
    return false;
  p.meshes.erase(it);
  return true;
}
bool Renderer::DestroyMaterial(uint64_t id) {
  auto &p = *impl;
  const auto it = p.materials.find(id);
  if (it == p.materials.end())
    return true;
  for (const auto &[key, mesh] : p.meshes)
    if (std::ranges::find(mesh.materials, it->second.handle) !=
        mesh.materials.end())
      return p.Fail("material still referenced by mesh", -1);
  if (!p.Usable() || !Wait() ||
      !p.Check(p.api.DestroyMaterial(it->second.handle), "DestroyMaterial"))
    return false;
  p.materials.erase(it);
  return true;
}
bool Renderer::DestroyLight(uint64_t id) {
  auto &p = *impl;
  const auto it = p.lights.find(id);
  if (it == p.lights.end())
    return true;
  if (!p.Usable() || !Wait() ||
      !p.Check(p.api.DestroyLight(it->second.handle), "DestroyLight"))
    return false;
  p.lights.erase(it);
  return true;
}

bool Renderer::Camera(const remixapi_CameraInfo &info) {
  auto &p = *impl;
  if (!p.Usable() || info.sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO)
    return p.Fail("Camera arguments", -1);
  p.camera_ready = p.Check(p.api.SetupCamera(&info), "SetupCamera");
  return p.camera_ready;
}
bool Renderer::Draw(uint64_t mesh, const remixapi_InstanceInfo &info) {
  auto &p = *impl;
  const auto it = p.meshes.find(mesh);
  if (!p.Usable() || !p.camera_ready || it == p.meshes.end() ||
      info.sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO ||
      !Finite(&info.transform.matrix[0][0], 12))
    return p.Fail("Draw arguments", -1);
  auto instance = info;
  instance.mesh = it->second.handle;
  return p.Check(p.api.DrawInstance(&instance), "DrawInstance");
}
bool Renderer::DrawLight(uint64_t id) {
  auto &p = *impl;
  const auto it = p.lights.find(id);
  if (!p.Usable() || !p.camera_ready || it == p.lights.end())
    return p.Fail("DrawLight arguments", -1);
  return p.Check(p.api.DrawLightInstance(it->second.handle),
                 "DrawLightInstance");
}
bool Renderer::Config(const char *name, const char *value) {
  auto &p = *impl;
  if (!p.Usable() || !name || !value)
    return p.Fail("Config arguments", -1);
  return p.Check(p.api.SetConfigVariable(name, value), "SetConfigVariable");
}
bool Renderer::Render() {
  auto &p = *impl;
  if (!p.Usable() || !p.camera_ready || !p.output_surface)
    return p.Fail("Render state", -1);
  MSG message{};
  while (PeekMessageW(&message, p.window, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Debug("Present begin");
  if (!Wait() || !p.Check(p.api.Present(nullptr), "Present"))
    return false;
  Debug("Copy output begin");
  if (!p.Check(p.api.dxvk_CopyRenderingOutput(
                   p.output_surface,
                   REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_FINAL_COLOR),
               "CopyRenderingOutput"))
    return false;
  Debug("Event issue begin");
  if (!p.CheckHr(p.done->Issue(D3DISSUE_END), "Issue completion event"))
    return false;
  p.pending = true;
  p.camera_ready = false;
  if (!Wait())
    return false;
  p.rendered = true;
  Debug("Render done");
  return true;
}
bool Renderer::Wait() {
  auto &p = *impl;
  if (!p.pending)
    return !p.failed;
  Debug("Wait begin");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    MSG message{};
    while (PeekMessageW(&message, p.window, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    BOOL complete{};
    const HRESULT hr =
        p.done->GetData(&complete, sizeof(complete), D3DGETDATA_FLUSH);
    if (hr == S_OK && complete) {
      p.pending = false;
      return true;
    }
    if (FAILED(hr))
      return p.CheckHr(hr, "GetData completion event");
    if (std::chrono::steady_clock::now() >= deadline)
      return p.Fail("render timeout", -1, true);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
Renderer::SharedOutput Renderer::Output() const {
  const auto &p = *impl;
  return p.Usable() && p.rendered ? SharedOutput{p.shared, p.width, p.height,
                                                 p.generation, p.adapter_luid}
                                  : SharedOutput{};
}
bool Renderer::Readback(std::span<uint16_t> pixels) {
  auto &p = *impl;
  if (!p.Usable() || !p.rendered ||
      pixels.size() != size_t(p.width) * p.height * 4 || !Wait())
    return p.Fail("Readback arguments", -1);
  if (!p.readback && !p.CheckHr(p.device->CreateOffscreenPlainSurface(
                                    p.width, p.height, D3DFMT_A16B16G16R16F,
                                    D3DPOOL_SYSTEMMEM, &p.readback, nullptr),
                                "Create readback surface"))
    return false;
  if (!p.CheckHr(p.device->GetRenderTargetData(p.output_surface, p.readback),
                 "GetRenderTargetData"))
    return false;
  D3DLOCKED_RECT locked{};
  if (!p.CheckHr(p.readback->LockRect(&locked, nullptr, D3DLOCK_READONLY),
                 "LockRect"))
    return false;
  for (uint32_t y = 0; y < p.height; ++y)
    std::memcpy(pixels.data() + size_t(y) * p.width * 4,
                static_cast<const char *>(locked.pBits) +
                    size_t(y) * locked.Pitch,
                size_t(p.width) * 8);
  return p.CheckHr(p.readback->UnlockRect(), "UnlockRect");
}

void Renderer::Shutdown() {
  auto &p = *impl;
  if (p.device && p.pending)
    Wait();
  // Release every app-owned D3D9 child before SDK Shutdown, which releases its
  // registered device itself. Never call Release on that device after Shutdown
  // has invalidated it.
  p.ReleaseOutput();
  Release(p.done);
  // Remix subclasses the HWND. Destroy it while its registered D3D9 device and
  // callback are both alive; SDK Shutdown otherwise leaves a dangling callback.
  if (p.window)
    DestroyWindow(p.window);
  p.window = nullptr;
  if (p.registered) {
    for (const auto &[id, mesh] : p.meshes)
      p.api.DestroyMesh(mesh.handle);
    for (const auto &[id, light] : p.lights)
      p.api.DestroyLight(light.handle);
    for (const auto &[id, material] : p.materials)
      p.api.DestroyMaterial(material.handle);
    p.api.Shutdown();
    p.device = nullptr;
    p.d3d = nullptr;
  } else {
    Release(p.device);
    Release(p.d3d);
  }
  p.meshes.clear();
  p.lights.clear();
  p.materials.clear();
  p.registered = p.pending = p.rendered = p.camera_ready = false;
  p.api = {};
  if (p.module)
    FreeLibrary(p.module);
  p.module = nullptr;
  if (p.dll_directory)
    RemoveDllDirectory(p.dll_directory);
  p.dll_directory = nullptr;
}
} // namespace BbRemix
