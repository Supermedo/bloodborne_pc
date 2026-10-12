// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone official renderer + Vulkan shared-output diagnostic. No game data.
#include "remix_vulkan.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void VkCheck(VkResult result, const char *message) {
  Require(result == VK_SUCCESS, message);
}
float Half(uint16_t h) {
  const float sign = h & 0x8000 ? -1.f : 1.f;
  const int exponent = (h >> 10) & 31;
  const int fraction = h & 1023;
  if (!exponent)
    return sign * std::ldexp(float(fraction), -24);
  if (exponent == 31)
    return fraction ? NAN : sign * INFINITY;
  return sign * std::ldexp(1.f + float(fraction) / 1024.f, exponent - 15);
}
struct Consumer {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  VkCommandPool pool{};
  uint32_t family{};
  Consumer(const BbRemix::Renderer::SharedOutput &output) {
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                nullptr,
                                "bb-remix-check",
                                2,
                                nullptr,
                                0,
                                VK_API_VERSION_1_2};
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    nullptr, 0, &app};
    VkCheck(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
    uint32_t count{};
    VkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr),
            "enumerate adapters");
    std::vector<VkPhysicalDevice> candidates(count);
    VkCheck(vkEnumeratePhysicalDevices(instance, &count, candidates.data()),
            "enumerate adapters");
    for (auto candidate : candidates) {
      VkPhysicalDeviceIDProperties id{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 properties{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
      vkGetPhysicalDeviceProperties2(candidate, &properties);
      if (id.deviceLUIDValid &&
          !std::memcmp(id.deviceLUID, output.adapter_luid.data(),
                       VK_LUID_SIZE)) {
        physical = candidate;
        break;
      }
    }
    Require(physical != VK_NULL_HANDLE,
            "Remix adapter missing in consumer instance");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    family =
        uint32_t(std::find_if(families.begin(), families.end(),
                              [](const auto &f) {
                                return f.queueFlags & VK_QUEUE_GRAPHICS_BIT;
                              }) -
                 families.begin());
    Require(family < count, "graphics queue missing");
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        nullptr,
        0,
        family,
        1,
        &priority};
    const char *extension = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
    const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                         nullptr,
                                         0,
                                         1,
                                         &queue_info,
                                         0,
                                         nullptr,
                                         1,
                                         &extension};
    VkCheck(vkCreateDevice(physical, &device_info, nullptr, &device),
            "vkCreateDevice external memory");
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    VkCheck(vkCreateCommandPool(device, &pool_info, nullptr, &pool),
            "vkCreateCommandPool");
  }
  ~Consumer() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (pool)
        vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
  std::vector<uint16_t> Read(const BbRemix::VulkanOutput &image, uint32_t width,
                             uint32_t height) {
    std::vector<uint16_t> pixels(size_t(width) * height * 4);
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  nullptr,
                                  0,
                                  pixels.size() * 2,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
    VkCheck(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t type{};
    for (; type < props.memoryTypeCount; ++type)
      if ((requirements.memoryTypeBits & (1u << type)) &&
          (props.memoryTypes[type].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
              (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        break;
    Require(type < props.memoryTypeCount, "host-coherent memory missing");
    const VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     nullptr, requirements.size, type};
    VkCheck(vkAllocateMemory(device, &alloc, nullptr, &memory),
            "readback allocation");
    VkCheck(vkBindBufferMemory(device, buffer, memory, 0), "readback bind");
    VkCommandBuffer command{};
    const VkCommandBufferAllocateInfo command_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCheck(vkAllocateCommandBuffers(device, &command_info, &command),
            "command allocation");
    const VkCommandBufferBeginInfo begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCheck(vkBeginCommandBuffer(command, &begin), "command begin");
    image.Acquire(command, family);
    const VkBufferImageCopy copy{
        0,         0,
        0,         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {0, 0, 0}, {width, height, 1}};
    vkCmdCopyImageToBuffer(command, image.Image(), VK_IMAGE_LAYOUT_GENERAL,
                           buffer, 1, &copy);
    image.Release(command, family);
    const VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                               VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
    VkCheck(vkEndCommandBuffer(command), "command end");
    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              nullptr,
                              0,
                              nullptr,
                              nullptr,
                              1,
                              &command};
    VkCheck(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE),
            "consumer submit");
    VkCheck(vkQueueWaitIdle(queue), "consumer completion");
    void *mapped{};
    VkCheck(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "consumer map");
    std::memcpy(pixels.data(), mapped, pixels.size() * 2);
    vkUnmapMemory(device, memory);
    vkFreeCommandBuffers(device, pool, 1, &command);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    return pixels;
  }
};
void Save(const std::filesystem::path &path,
          const std::vector<uint16_t> &pixels, unsigned w, unsigned h) {
  std::ofstream file(path, std::ios::binary);
  Require(bool(file), "image output open failed");
  file << "P6\n" << w << ' ' << h << "\n255\n";
  for (size_t i = 0; i < pixels.size(); i += 4)
    for (unsigned c = 0; c < 3; ++c) {
      const auto value =
          uint8_t(std::clamp(Half(pixels[i + c]), 0.f, 1.f) * 255.f + 0.5f);
      file.put(char(value));
    }
  Require(bool(file), "image output write failed");
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // Official DXVK deterministic submission mode: flush SDK work before polling
  // its D3D9 event. This process is dedicated to Remix, never the game process.
  SetEnvironmentVariableW(L"DXVK_EXPLICIT_FLUSH", L"1");
  if (argc < 2 || argc > 3) {
    std::puts("Usage: bb-remix-check.exe <absolute path to official "
              ".trex/d3d9.dll> [output.ppm]\n"
              "Synthetic SDK scene diagnostic; does not enable Bloodborne path "
              "tracing.");
    return 2;
  }
  try {
    BbRemix::Renderer renderer;
    Require(renderer.Initialize(std::filesystem::absolute(argv[1]), 320, 180),
            renderer.Error().c_str());
    Require(renderer.Config("rtx.graphicsPreset", "4"),
            renderer.Error().c_str());
    Require(renderer.Config("rtx.integrateIndirectMode", "0"),
            renderer.Error().c_str());
    Require(renderer.Config("rtx.upscalerType", "0"), renderer.Error().c_str());
    Require(renderer.Config("rtx.enableVsync", "0"), renderer.Error().c_str());
    auto opaque = BbRemix::Renderer::OpaqueDefaults();
    opaque.albedoConstant = {1, 0.03f, 0.03f};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = 0.7f;
    auto material = BbRemix::Renderer::MaterialDefaults(101);
    material.pNext = &opaque;
    material.emissiveIntensity = 0.5f;
    material.emissiveColorConstant = {1, 0, 0};
    Require(renderer.Material(material, 1), renderer.Error().c_str());
    std::array<remixapi_HardcodedVertex, 3> vertices{};
    const float positions[3][3]{{-2, -2, 5}, {0, 2, 5}, {2, -2, 5}};
    for (unsigned i = 0; i < 3; ++i) {
      std::memcpy(vertices[i].position, positions[i], sizeof positions[i]);
      vertices[i].normal[2] = -1;
      vertices[i].color = 0xffffffff;
    }
    const uint32_t indices[]{0, 1, 2};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = nullptr;
    surface.indices_count = 0;
    surface.material = renderer.MaterialHandle(101);
    remixapi_MeshInfo mesh{REMIXAPI_STRUCT_TYPE_MESH_INFO, nullptr, 102,
                           &surface, 1};
    Require(renderer.Mesh(mesh, 1), renderer.Error().c_str());
    Require(renderer.Mesh(mesh, 1), "cached mesh reuse");
    Require(!renderer.DestroyMaterial(101),
            "referenced material deletion must fail");
    const uint32_t invalid[]{0, 1, 9};
    surface.indices_values = invalid;
    surface.indices_count = 3;
    Require(!renderer.Mesh(mesh, 2),
            "invalid vertex index must fail before SDK call");
    surface.indices_values = nullptr;
    surface.indices_count = 0;

    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 1, 0};
    sphere.radius = 0.5f;
    sphere.volumetricRadianceScale = 1;
    remixapi_LightInfo light{
        REMIXAPI_STRUCT_TYPE_LIGHT_INFO, &sphere, 103, {20, 20, 20}};
    Require(renderer.Light(light, 1), renderer.Error().c_str());
    const auto frame = [&](uint32_t w, uint32_t h, float x) {
      remixapi_CameraInfoParameterizedEXT camera_parameters{};
      camera_parameters.sType =
          REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      camera_parameters.position = {x, 0, 0};
      camera_parameters.forward = {0, 0, 1};
      camera_parameters.up = {0, 1, 0};
      camera_parameters.right = {1, 0, 0};
      camera_parameters.fovYInDegrees = 70;
      camera_parameters.aspect = float(w) / h;
      camera_parameters.nearPlane = 0.1f;
      camera_parameters.farPlane = 100;
      remixapi_CameraInfo camera{REMIXAPI_STRUCT_TYPE_CAMERA_INFO,
                                 &camera_parameters};
      Require(renderer.Camera(camera), renderer.Error().c_str());
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
      instance.doubleSided = true;
      Require(renderer.Draw(102, instance), renderer.Error().c_str());
      Require(renderer.DrawLight(103), renderer.Error().c_str());
      Require(renderer.Render(), renderer.Error().c_str());
    };
    // Remix may return successful Presents while its async shader prewarm still
    // produces black output. Wait for actual geometry, with a bounded deadline.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(90);
    std::vector<uint16_t> warmup(320u * 180u * 4);
    unsigned warmup_frames = 0;
    for (;;) {
      frame(320, 180, 0);
      Require(renderer.Readback(warmup), renderer.Error().c_str());
      const float red = Half(warmup[(90u * 320u + 160u) * 4]);
      if (++warmup_frames % 32 == 0) {
        float maximum = 0;
        for (size_t i = 0; i < warmup.size(); i += 4)
          maximum = std::max(maximum, Half(warmup[i]));
        std::printf("Warmup: %u frames, center red %.5f, max red %.5f\n",
                    warmup_frames, red, maximum);
      }
      if (red > 0.05f)
        break;
      Require(std::chrono::steady_clock::now() < deadline,
              "shader warmup produced no geometry in 90 seconds");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Consumer consumer(renderer.Output());
    BbRemix::VulkanOutput imported(consumer.physical, consumer.device);
    const auto verify = [&](uint32_t w, uint32_t h) {
      Require(imported.Import(renderer.Output()), imported.Error().c_str());
      const auto pixels = consumer.Read(imported, w, h);
      std::vector<uint16_t> reference(pixels.size());
      Require(renderer.Readback(reference), renderer.Error().c_str());
      Require(pixels == reference,
              "shared Vulkan pixels differ from SDK output");
      const size_t center = (size_t(h / 2) * w + w / 2) * 4;
      const float red = Half(pixels[center]), green = Half(pixels[center + 1]),
                  blue = Half(pixels[center + 2]);
      std::printf(
          "SDK pixels: center RGB %.6f %.6f %.6f, shared import identical\n",
          red, green, blue);
      Require(std::isfinite(red) && red > 0.05f && red > green * 1.15f &&
                  red > blue * 1.15f,
              "official renderer did not produce expected red geometry");
      if (argc == 3)
        Save(argv[2], pixels, w, h);
    };
    verify(320, 180);
    // Same height: exercise the one-axis resize that SDK Present's reset
    // misses.
    imported.Reset();
    Require(renderer.Resize(400, 180), renderer.Error().c_str());
    for (unsigned i = 0; i < 3; ++i)
      frame(400, 180, 0);
    verify(400, 180);
    imported.Reset();
    // Revision replaces the mesh without invalidating its material, then
    // deletion order.
    surface.indices_values = indices;
    surface.indices_count = 3;
    Require(renderer.Mesh(mesh, 2), renderer.Error().c_str());
    for (unsigned i = 0; i < 3; ++i)
      frame(400, 180, 0);
    verify(400,
           180); // actual 32-bit indexed SDK geometry, not just API acceptance
    imported.Reset();
    Require(renderer.DestroyMesh(102), renderer.Error().c_str());
    Require(renderer.DestroyLight(103), renderer.Error().c_str());
    Require(renderer.DestroyMaterial(101), renderer.Error().c_str());
    renderer.Shutdown();
    std::puts("RTX Remix SDK PASS: official rendered pixels, shared Vulkan "
              "output, caching, bounds, resize, teardown");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "RTX Remix SDK CHECK FAILED: %s\n", error.what());
    return 1;
  }
}
