// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <remix/remix_c.h>
#include <span>
#include <string>

namespace BbRemix {

// All methods run on one render thread. No SDK call is made during normal
// startup. Resource identifiers must come from the scene adapter and remain
// stable across frames.
class Renderer {
public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  // Zero initialization means VK_COMPARE_OP_NEVER in this SDK, making opaque
  // meshes invisible. Use these defaults before filling game material values.
  static remixapi_MaterialInfoOpaqueEXT OpaqueDefaults();
  static remixapi_MaterialInfo MaterialDefaults(uint64_t id);

  bool Initialize(const std::filesystem::path &runtime_dll, uint32_t width,
                  uint32_t height, std::array<uint8_t, 8> adapter = {});
  bool
  Resize(uint32_t width,
         uint32_t height); // consumer must release its imported image first
  bool Material(const remixapi_MaterialInfo &info, uint64_t revision);
  bool Mesh(const remixapi_MeshInfo &info, uint64_t revision);
  bool Light(const remixapi_LightInfo &info, uint64_t revision);
  remixapi_MaterialHandle MaterialHandle(uint64_t id) const;
  bool DestroyMesh(uint64_t id);
  bool
  DestroyMaterial(uint64_t id); // refuses deletion while a mesh references it
  bool DestroyLight(uint64_t id);

  bool Camera(const remixapi_CameraInfo &info);
  bool Draw(uint64_t mesh, const remixapi_InstanceInfo &instance);
  bool DrawLight(uint64_t id);
  bool Config(const char *name, const char *value);
  bool Render(); // SDK Present + CopyRenderingOutput + bounded event wait
  bool Wait();
  void Shutdown();

  struct SharedOutput {
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT; do not CloseHandle.
    // Lifetime is that of the D3D9 texture, not the SDK's internal swapchain.
    HANDLE memory{};
    uint32_t width{}, height{};
    uint64_t generation{};
    std::array<uint8_t, 8> adapter_luid{};
    // Vulkan import must use R16G16B16A16_SFLOAT and the same physical adapter.
  };
  SharedOutput Output() const;
  // Diagnostic readback only. Gameplay consumers should import shared memory
  // instead.
  bool Readback(std::span<uint16_t> rgba_half);
  const std::string &Error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace BbRemix
