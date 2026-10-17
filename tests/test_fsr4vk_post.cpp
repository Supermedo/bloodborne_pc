// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only guard and exact transformation checks; no Vulkan device or game data.
#include "video_core/renderer_vulkan/fsr4vk_post.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace Vulkan::Fsr4VkPost;

static std::vector<uint32_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() % 4 != 0) {
        throw std::runtime_error("invalid shader file");
    }
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4));
    if (!file) throw std::runtime_error("shader read failed");
    return words;
}

int main(int argc, char** argv) {
    assert(Eligible(0x1002, 0x7550, 3840, 2160, 2560, 1440, 1, false));
    assert(!Eligible(0x10de, 0x7550, 3840, 2160, 2560, 1440, 1, false));
    assert(!Eligible(0x1002, 0x744c, 3840, 2160, 2560, 1440, 1, false));
    assert(!Eligible(0x1002, 0x7550, 3840, 2160, 2560, 1440, 1, true));
    for (int preset : {0, 2, 3, 4})
        assert(!Eligible(0x1002, 0x7550, 3840, 2160, 2560, 1440, preset, false));
    assert(!Eligible(0x1002, 0x7550, 2560, 1440, 1707, 960, 1, false));
    assert(!Eligible(0x1002, 0x7550, 3840, 2160, 1920, 1080, 1, false));
    assert(Rewrite({}).empty());
    assert(Rewrite(std::vector<uint32_t>(145456 / 4)).empty());
    if (argc == 3) {
        auto original = Read(argv[1]);
        const auto expected = Read(argv[2]);
        const auto optimized = Rewrite(original);
        assert(!optimized.empty() && optimized == expected);
        assert(optimized[0] == original[0] && optimized[1] == original[1]);
        original.back() ^= 1;
        assert(Rewrite(original).empty());
        original.pop_back();
        assert(Rewrite(original).empty());
        std::puts("PASS: exact generated shader, unknown/mutated/truncated module rejection");
    } else {
        assert(argc == 1);
        std::puts("Pass ORIGINAL.spv OPTIMIZED.spv to also check the pinned transformation.");
    }
    std::puts("PASS: post-shader GPU/resolution/preset/color guards");
}
