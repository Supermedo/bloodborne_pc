#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/rt/rt_gi.h"
#include "video_core/renderer_vulkan/rt/vk_ray_tracing.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class SceneTargets;

class DlssGuides {
public:
    DlssGuides(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
               VideoCore::TextureCache& texture_cache, SceneTargets& scene_targets);
    ~DlssGuides();

    struct Camera {
        std::array<float, 4> projection{};
        Rt::Affine inv_view{};
        std::array<float, 2> jitter{};
        bool valid{};
    };

    bool Record(vk::CommandBuffer cmdbuf, const RayTracing::RrSource& source,
                const Dlss::Resource& depth, bool reduced, const Camera& camera,
                Dlss::RrGuides& out);
    bool Linearize(vk::CommandBuffer cmdbuf, const Dlss::Resource& color, Dlss::Resource& out);
    void Tonemap(vk::CommandBuffer cmdbuf, const Dlss::Resource& output);

private:
    struct Params {
        std::array<float, 4> proj;
        std::array<std::array<float, 4>, 3> world_rows;
        std::array<float, 4> size_jitter;
        std::array<u32, 4> cfg;
    };
    static_assert(sizeof(Params) == 96);

    bool EnsureResources(u32 width, u32 height);
    bool EnsurePipeline();
    bool EnsureTonemap();
    void DispatchTonemap(vk::CommandBuffer cmdbuf, vk::Pipeline pass, vk::ImageView src,
                         vk::ImageView dst, u32 w, u32 h);
    bool Bind(VideoCore::ImageId id, bool reduced, u32 width, u32 height, vk::Image& image,
              vk::ImageView& view);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    VideoCore::TextureCache& texture_cache;
    SceneTargets& scene_targets;
    u32 width = 0, height = 0;
    bool pipeline_failed = false;
    bool printed = false;
    u32 hit_logged = 0;
    std::array<VideoCore::UniqueImage, 3> images;
    std::array<vk::UniqueImageView, 3> views;
    bool images_fresh = true;
    vk::UniqueDescriptorSetLayout layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    bool tonemap_failed = false;
    vk::UniqueDescriptorSetLayout tonemap_layout;
    vk::UniquePipelineLayout tonemap_pipeline_layout;
    vk::UniquePipeline linearize_pipeline;
    vk::UniquePipeline tonemap_pipeline;
    VideoCore::UniqueImage linear_image;
    vk::UniqueImageView linear_view;
    u32 linear_width = 0, linear_height = 0;
    bool linear_fresh = true;
};

}
