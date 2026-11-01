// SPDX-License-Identifier: GPL-2.0-or-later
// Included by the motion-shader fixture to reuse Buffer/Image/read helpers.
static int MotionHistorySsboGpuTest(const std::filesystem::path& dir) {
    try {
        using namespace Vulkan;
        Instance instance(0, std::getenv("BB_OMV_TEST_VALIDATION") != nullptr);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetInstance());
        VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.GetDevice());
        const auto device = instance.GetDevice();
        const auto allocator = instance.GetAllocator();
        Scheduler scheduler(instance, std::getenv("BB_OMV_TEST_THREADED") != nullptr);
        constexpr size_t P = Shader::MotionVectors::PositionsPerHalf;
        constexpr size_t Slots = Shader::MotionVectors::PositionSlots, Bytes = Slots * 16;
        Buffer history(allocator, Bytes,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer indices(allocator, 3072 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        Buffer indirect(allocator, sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        *static_cast<VkDrawIndexedIndirectCommand*>(indirect.mapped) = {3072, 2, 0, 0, 0};
        Buffer dummy_buffer(allocator, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Buffer pixels(allocator, 64 * 64 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        BbSettings::Get().object_motion = true;
        BbSettings::Get().upscaler = BbSettings::UpscalerFsr3;
        auto owner = ObjectMotionParameterTest::Ssbo(instance, scheduler);
        auto& motion = *owner;
        Need(motion.Enabled(), "native full SSBO unavailable");
        auto* parameters = static_cast<uint32_t*>(ObjectMotionParameterTest::Mapped(motion));
        const vk::Buffer native = ObjectMotionParameterTest::HistoryBuffer(motion);
        for (unsigned i = 0; i < 3072; ++i)
            static_cast<uint32_t*>(indices.mapped)[i] = i % 3;
        WriteMotionShaders(dir, Shader::MotionVectors::params_address,
                           Shader::MotionVectors::positions_address);
        std::array<vk::UniqueShaderModule, 2> modules;
        for (unsigned i = 0; i < 2; ++i) {
            const auto code = Read(dir / (i ? "fragment-motion.spv" : "vertex-motion.spv"));
            auto [r, s] = device.createShaderModuleUnique(
                {.codeSize = code.size() * 4, .pCode = code.data()});
            Need(r == vk::Result::eSuccess, "full SSBO module");
            modules[i] = std::move(s);
        }
        const bool push0 = std::getenv("BB_OMV_TEST_SET0_PUSH") != nullptr;
        const vk::DescriptorSetLayoutBinding dummy{7, vk::DescriptorType::eStorageBuffer, 1,
                                                   vk::ShaderStageFlagBits::eVertex};
        auto [er, empty] = device.createDescriptorSetLayoutUnique(
            {.flags = push0 ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                            : vk::DescriptorSetLayoutCreateFlags{},
             .bindingCount = 1,
             .pBindings = &dummy});
        Need(er == vk::Result::eSuccess, "set0 fixture layout");
        auto motion_layout = CreateMotionParameterLayout(device);
        const std::array layouts{*empty, *motion_layout};
        const vk::PushConstantRange range{vk::ShaderStageFlagBits::eVertex |
                                              vk::ShaderStageFlagBits::eFragment,
                                          0, sizeof(Shader::PushData)};
        auto [lr, layout] = device.createPipelineLayoutUnique({.setLayoutCount = 2,
                                                               .pSetLayouts = layouts.data(),
                                                               .pushConstantRangeCount = 1,
                                                               .pPushConstantRanges = &range});
        Need(lr == vk::Result::eSuccess, "full SSBO pipeline layout");
        // A different set0 layout disturbs a previously bound set1. Exercise the
        // explicit rebind required after BindResources, for push and ordinary sets.
        const vk::DescriptorSetLayoutBinding changed{8, vk::DescriptorType::eStorageBuffer, 1,
                                                     vk::ShaderStageFlagBits::eVertex};
        auto [cr, changed_set0] = device.createDescriptorSetLayoutUnique(
            {.flags = push0 ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                            : vk::DescriptorSetLayoutCreateFlags{},
             .bindingCount = 1, .pBindings = &changed});
        Need(cr == vk::Result::eSuccess, "different set0 layout");
        const vk::DescriptorSetLayout changed_raw = *changed_set0;
        auto [clr, changed_layout] = device.createPipelineLayoutUnique(
            {.setLayoutCount = 1, .pSetLayouts = &changed_raw});
        Need(clr == vk::Result::eSuccess, "different pipeline layout");
        vk::UniqueDescriptorPool set0_pool;
        std::vector<vk::DescriptorSet> set0_sets;
        const vk::DescriptorBufferInfo dummy_info{vk::Buffer(dummy_buffer.buffer), 0, 32};
        if (!push0) {
            const vk::DescriptorPoolSize size{vk::DescriptorType::eStorageBuffer, 2};
            auto [r, pool] = device.createDescriptorPoolUnique(
                {.maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size});
            Need(r == vk::Result::eSuccess, "set0 pool");
            set0_pool = std::move(pool);
            const std::array allocate_layouts{*empty, *changed_set0};
            auto [a, sets] = device.allocateDescriptorSets({.descriptorPool = *set0_pool,
                .descriptorSetCount = 2, .pSetLayouts = allocate_layouts.data()});
            Need(a == vk::Result::eSuccess, "set0 allocation");
            set0_sets = std::move(sets);
            for (unsigned i = 0; i < 2; ++i) {
                const vk::WriteDescriptorSet write{.dstSet = set0_sets[i], .dstBinding = 7 + i,
                    .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                    .pBufferInfo = &dummy_info};
                device.updateDescriptorSets(write, {});
            }
        }
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex, .module = *modules[0], .pName = "main"},
            vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                              .module = *modules[1],
                                              .pName = "main"}};
        const vk::PipelineVertexInputStateCreateInfo vertex{};
        const vk::PipelineInputAssemblyStateCreateInfo input{
            .topology = vk::PrimitiveTopology::eTriangleList};
        const vk::Viewport vp{0, 0, 64, 64, 0, 1};
        const vk::Rect2D scissor{{0, 0}, {64, 64}};
        const vk::PipelineViewportStateCreateInfo viewport{
            .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scissor};
        const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
                                                              .cullMode =
                                                                  vk::CullModeFlagBits::eNone,
                                                              .lineWidth = 1};
        const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples =
                                                            vk::SampleCountFlagBits::e1};
        std::array<vk::PipelineColorBlendAttachmentState, 8> blends{};
        for (auto& b : blends)
            b.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 8,
                                                          .pAttachments = blends.data()};
        std::array<vk::Format, 8> formats;
        formats.fill(vk::Format::eR32G32B32A32Sfloat);
        const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 8,
                                                        .pColorAttachmentFormats = formats.data()};
        auto [pr, pipeline] = device.createGraphicsPipelineUnique(
            {}, vk::GraphicsPipelineCreateInfo{.pNext = &rendering,
                                               .stageCount = 2,
                                               .pStages = stages.data(),
                                               .pVertexInputState = &vertex,
                                               .pInputAssemblyState = &input,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &ms,
                                               .pColorBlendState = &blend,
                                               .layout = *layout});
        Need(pr == vk::Result::eSuccess, "full SSBO graphics pipeline");
        std::vector<std::unique_ptr<Image>> images;
        for (unsigned i = 0; i < 8; ++i)
            images.push_back(std::make_unique<Image>(allocator, device));
        const std::array<std::array<float, 4>, 3> positions{
            {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}};
        const auto execute = [&](const Shader::PushData& data, bool upload,
                                 const Shader::PushData* second = nullptr, bool draw_indirect = false) {
            const bool twice = second != nullptr;
            const Shader::PushData next = second ? *second : Shader::PushData{};
            motion.BindParameters(*layout);
            scheduler.Record([&](vk::CommandBuffer cmd) {
                for (bool incompatible : {true, false}) {
                    const auto current = incompatible ? *changed_layout : *layout;
                    if (push0) {
                        const vk::WriteDescriptorSet write{.dstBinding = incompatible ? 8u : 7u,
                            .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                            .pBufferInfo = &dummy_info};
                        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, current, 0, write);
                    } else {
                        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, current, 0,
                                               set0_sets[incompatible ? 1 : 0], {});
                    }
                }
            });
            motion.BindParameters(*layout);
            scheduler.Record([&, data, upload, twice, next, draw_indirect](vk::CommandBuffer cmd) {
                if (upload) {
                    const vk::BufferCopy copy{0, 0, Bytes};
                    cmd.copyBuffer(vk::Buffer(history.buffer), native, copy);
                    const vk::MemoryBarrier2 ready{
                        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
                        .dstAccessMask =
                            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite};
                    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ready});
                }
                std::array<vk::ImageMemoryBarrier2, 8> barriers{};
                std::array<vk::RenderingAttachmentInfo, 8> attachments{};
                for (unsigned i = 0; i < 8; ++i) {
                    barriers[i] = {
                        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                        .oldLayout = vk::ImageLayout::eUndefined,
                        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                        .image = vk::Image(images[i]->image),
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                    attachments[i] = {.imageView = *images[i]->view,
                                      .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                      .loadOp = vk::AttachmentLoadOp::eClear,
                                      .storeOp = vk::AttachmentStoreOp::eStore,
                                      .clearValue = vk::ClearValue{vk::ClearColorValue{
                                          std::array<float, 4>{-99, -99, -99, -99}}}};
                }
                cmd.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 8, .pImageMemoryBarriers = barriers.data()});
                cmd.beginRendering({.renderArea = scissor,
                                    .layerCount = 1,
                                    .colorAttachmentCount = 8,
                                    .pColorAttachments = attachments.data()});
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
                cmd.bindIndexBuffer(vk::Buffer(indices.buffer), 0, vk::IndexType::eUint32);
                cmd.pushConstants(*layout, range.stageFlags, 0, sizeof(data), &data);
                if (draw_indirect)
                    cmd.drawIndexedIndirect(vk::Buffer(indirect.buffer), 0, 1,
                                             sizeof(VkDrawIndexedIndirectCommand));
                else
                    cmd.drawIndexed(3072, 2, 0, 0, 0);
                if (twice) {
                    cmd.pushConstants(*layout, range.stageFlags, 0, sizeof(next), &next);
                    cmd.drawIndexed(3072, 2, 0, 0, 0);
                }
                cmd.endRendering();
                const vk::MemoryBarrier2 ready{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask =
                        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask =
                        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite};
                const vk::ImageMemoryBarrier2 output{
                    .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .image = vk::Image(images[7]->image),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                                      .pMemoryBarriers = &ready,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &output});
                const vk::BufferCopy copy{0, 0, Bytes};
                cmd.copyBuffer(native, vk::Buffer(history.buffer), copy);
                const vk::BufferImageCopy image_copy{
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {64, 64, 1}};
                cmd.copyImageToBuffer(vk::Image(images[7]->image),
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      vk::Buffer(pixels.buffer), image_copy);
                const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                              .dstAccessMask = vk::AccessFlagBits2::eHostRead};
                cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host});
            });
            scheduler.Finish();
        };
        const auto check_pixel = [&](bool valid, float dx) {
            const float* pixel = static_cast<float*>(pixels.mapped) + (32 * 64 + 32) * 4;
            Need(std::abs(pixel[2] - (valid ? 1.f : 0.f)) < .001f,
                 "SSBO validity readback mismatch");
            Need(std::abs(pixel[0] - dx) < .001f, "SSBO motion vector readback mismatch");
        };
        // Indirect draws have no validated geometry/history range and use entry zero.
        // They must still bind set1 and produce camera-only fallback without writes.
        std::memset(parameters, 0, Shader::MotionVectors::ParamSlots * 32);
        std::memset(history.mapped, 0xcd, Bytes);
        execute(Shader::PushData{}, true, nullptr, true);
        check_pixel(false, 0.f);
        const auto* zero_history = static_cast<const uint32_t*>(history.mapped);
        for (size_t word = 0; word < Bytes / 4; ++word)
            Need(zero_history[word] == 0xcdcdcdcd, "indirect zero record wrote history");
        std::puts("PASS SSBO indirect zero parameter after incompatible set0 layout/rebind");
        for (unsigned test = 0; test < 14; ++test) {
            std::memset(parameters, 0, Shader::MotionVectors::ParamSlots * 32);
            std::memset(history.mapped, 0xcd, Bytes);
            uint32_t store = 1, load = 32, flags = 3;
            if (test == 2) {
                store = UINT32_MAX;
                flags = 1;
            }
            if (test == 3) {
                load = UINT32_MAX;
                flags = 2;
            }
            if (test == 7) {
                store = P + 1;
                load = P + 32;
            }
            if (test == 8)
                store = P + 1;
            if (test == 9)
                load = P + 32;
            if (test == 10)
                store = 2 * P - 5;
            if (test == 11) {
                store = P - 5;
                load = P + 32;
            }
            if (test == 12) {
                store = P + 1;
                load = P - 5;
            }
            if (test == 13)
                load = 2 * P - 5;
            Shader::PushData push{};
            push.xscale = 32;
            push.yscale = 32;
            push.motion_param = test == 5 ? Shader::MotionVectors::ParamSlots - 1 : 1;
            const std::array<uint32_t, 8> record{
                store, load, 3, flags, test == 4 ? 100U : 0U, test == 4 ? 100U : 0U, 2, 0};
            std::memcpy(parameters + push.motion_param * 8, record.data(), 32);
            if (test == 1)
                push.motion_param = UINT32_MAX;
            if (test == 6)
                push.motion_param = 0;
            const bool source = load && uint64_t(load) + 6 <= Slots;
            if (source)
                for (unsigned i = 0; i < 6; ++i) {
                    auto v = positions[i % 3];
                    v[0] += .25f;
                    std::memcpy(static_cast<char*>(history.mapped) + (load + i) * 16, v.data(), 16);
                }
            execute(push, true);
            const bool decode = test != 1 && test != 6 && test != 4;
            const bool wrote = decode && (flags & 1) && store && uint64_t(store) + 6 <= Slots;
            const bool read = decode && (flags & 2) && source && true;
            const auto* words = static_cast<const uint32_t*>(history.mapped);
            for (size_t slot = 0; slot < Slots; ++slot)
                for (unsigned c = 0; c < 4; ++c) {
                    uint32_t expected = 0xcdcdcdcd;
                    if (source && slot >= load && slot < uint64_t(load) + 6) {
                        float v = positions[(slot - load) % 3][c];
                        if (c == 0)
                            v += .25f;
                        expected = std::bit_cast<uint32_t>(v);
                    }
                    if (wrote && slot >= store && slot < uint64_t(store) + 6)
                        expected = std::bit_cast<uint32_t>(positions[(slot - store) % 3][c]);
                    Need(words[slot * 4 + c] == expected, "SSBO history or sentinel mismatch");
                }
            check_pixel(read, read ? 8.f : 0.f);
            std::printf("PASS SSBO bounds mode=%s case=%u set0=%s recorder=%s\n", "ssbo", test,
                        push0 ? "push" : "normal",
                        scheduler.IsRecordingDeferred() ? "threaded" : "direct");
        }
        // Real CPU history/ring/tick path, no host-generated previous positions.
        std::memset(history.mapped, 0xcd, Bytes);
        for (unsigned frame = 0; frame < 12; ++frame) {
            motion.OnFrameStart();
            Shader::PushData push{};
            push.xscale = 32;
            push.yscale = 32;
            const ObjectMotion::DrawInfo draw{.shader = 7,
                                              .geometry = 8,
                                              .indices = 9,
                                              .topology = 10,
                                              .index_count = 3072,
                                              .instances = 2,
                                              .vertices = {0, 3}};
            push.motion_param = motion.PrepareDraw(draw);
            Shader::PushData next = push;
            next.motion_param = motion.PrepareDraw(draw);
            Need(push.motion_param != 0, "real history allocation failed");
            const auto* record = parameters + push.motion_param * 8;
            Need(record[1] == (frame ? 1 + uint32_t(frame & 1) * P : 0),
                 "previous half/ring mismatch");
            Need(parameters[next.motion_param * 8] == record[0] + 6,
                 "same-frame disjoint history ranges mismatch");
            execute(push, frame == 0, &next);
            const auto* words = static_cast<const uint32_t*>(history.mapped);
            for (size_t slot = 0; slot < Slots; ++slot)
                for (unsigned c = 0; c < 4; ++c) {
                    uint32_t expected = 0xcdcdcdcd;
                    if (slot >= P + 1 && slot < P + 13)
                        expected = std::bit_cast<uint32_t>(positions[(slot - P - 1) % 3][c]);
                    if (frame && slot >= 1 && slot < 13)
                        expected = std::bit_cast<uint32_t>(positions[(slot - 1) % 3][c]);
                    Need(words[slot * 4 + c] == expected, "real SSBO ping-pong history mismatch");
                }
            check_pixel(frame > 0 && true, 0.f);
            std::printf("PASS SSBO frame mode=%s frame=%u param=%u store=%u load=%u "
                        "(native ring/barrier)\n",
                        "ssbo", frame, push.motion_param, record[0], record[1]);
        }
        scheduler.Finish();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL SSBO history: %s\n", e.what());
        return 1;
    }
}
