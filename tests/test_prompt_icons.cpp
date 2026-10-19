// bbport: the keyboard and mouse key-guide icons (gpu/shim/bbport_prompt_icons.cpp).
// Renders sample inputs, checks the encoder's error against the drawn pixels with this file's
// own BC7 decoder (modes 5 and 6), and with an output folder writes <name>.rgba (drawn) and
// <name>.bc7 (encoded, tiled) for viewing.
#include "bbport_prompt_icons.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace BbPromptIcons;

namespace {

uint32_t Read(const uint8_t* block, int& at, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; ++i, ++at) v |= ((block[at >> 3] >> (at & 7)) & 1u) << i;
    return v;
}
int Interp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

/// Decodes a mode 5 or mode 6 BC7 block into 16 RGBA pixels (the only modes the encoder writes).
void DecodeBlock(const uint8_t* block, int out[16][4]) {
    static const int w2[4] = {0, 21, 43, 64}, w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    int at = 0;
    int mode = 0;
    while (!Read(block, at, 1)) ++mode;
    if (mode == 6) {
        int e[2][4];
        for (int k = 0; k < 4; ++k) { e[0][k] = Read(block, at, 7); e[1][k] = Read(block, at, 7); }
        const int p0 = Read(block, at, 1), p1 = Read(block, at, 1);
        for (int k = 0; k < 4; ++k) { e[0][k] = e[0][k] << 1 | p0; e[1][k] = e[1][k] << 1 | p1; }
        for (int i = 0; i < 16; ++i) {
            const int idx = Read(block, at, i == 0 ? 3 : 4);
            for (int k = 0; k < 4; ++k) out[i][k] = Interp(e[0][k], e[1][k], w4[idx]);
        }
    } else {
        assert(mode == 5);
        assert(Read(block, at, 2) == 0);
        int e[2][3];
        for (int k = 0; k < 3; ++k) {
            e[0][k] = Read(block, at, 7); e[1][k] = Read(block, at, 7);
            e[0][k] = e[0][k] << 1 | e[0][k] >> 6; e[1][k] = e[1][k] << 1 | e[1][k] >> 6;
        }
        const int a0 = Read(block, at, 8), a1 = Read(block, at, 8);
        for (int i = 0; i < 16; ++i) {
            const int idx = Read(block, at, i == 0 ? 1 : 2);
            for (int k = 0; k < 3; ++k) out[i][k] = Interp(e[0][k], e[1][k], w2[idx]);
        }
        for (int i = 0; i < 16; ++i) out[i][3] = Interp(a0, a1, w2[Read(block, at, i == 0 ? 1 : 2)]);
    }
    assert(at == 128);
}

int Tiled(int bx, int by) {
    int out = 0;
    for (int bit = 0; bit < 3; ++bit) out |= ((bx >> bit) & 1) << (2 * bit) | ((by >> bit) & 1) << (2 * bit + 1);
    return out;
}

/// Mean squared error of the visible colour (weighted by alpha) and of alpha.
double Error(const Pixels& drawn, const std::array<uint8_t, Bytes>& encoded) {
    double sum = 0;
    for (int by = 0; by < 8; ++by) {
        for (int bx = 0; bx < 8; ++bx) {
            int px[16][4];
            DecodeBlock(&encoded[Tiled(bx, by) * 16], px);
            for (int i = 0; i < 16; ++i) {
                const uint8_t* d = &drawn[((by * 4 + i / 4) * Size + bx * 4 + i % 4) * 4];
                for (int k = 0; k < 3; ++k) sum += std::pow((px[i][k] - d[k]) * d[3] / 255.0, 2);
                sum += std::pow(px[i][3] - d[3], 2);
            }
        }
    }
    return sum / (Size * Size * 4);
}

} // namespace

int main(int argc, char** argv) {
    std::ifstream font_file(BB_TEST_FONT, std::ios::binary);
    std::vector<unsigned char> font((std::istreambuf_iterator<char>(font_file)), {});
    assert(SetFont(font.data()));

    assert(KeyLabel("e") == "E" && KeyLabel("space") == "Space" && KeyLabel("lshift") == "Shift");
    assert(KeyLabel("f11") == "F11" && KeyLabel("kp5") == "Num5" && KeyLabel("leftbutton").empty());

    const std::vector<std::pair<std::string, Inputs>> samples = {
        {"e", {"e"}}, {"space", {"space"}}, {"escape", {"escape"}}, {"lshift", {"lshift"}},
        {"tab", {"tab"}}, {"leftbutton", {"leftbutton"}}, {"rightbutton", {"rightbutton"}},
        {"middlebutton", {"middlebutton"}}, {"wheelup", {"mousewheelup"}}, {"side", {"sidebuttonback"}},
        {"shift_click", {"lshift+leftbutton"}}, {"ctrl_e", {"lctrl+e"}}, {"wasd", {"w", "a", "s", "d"}},
        {"mouse", {"mouse"}}, {"updown", {"up", "down"}}, {"none", {}}, {"f11", {"f11"}},
        {"dpad", {"up", "down", "left", "right"}}, {"leftright", {"left", "right"}}, {"ik", {"i", "k"}},
    };
    double worst = 0;
    for (const auto& [name, inputs] : samples) {
        const Pixels drawn = Draw(inputs);
        const auto encoded = Encode(drawn);
        assert(Encode(drawn) == encoded);  // deterministic: the same icon every start
        const double error = Error(drawn, encoded);
        worst = std::max(worst, error);
        std::printf("%-14s mse %6.1f\n", name.c_str(), error);
        if (argc > 1) {
            std::ofstream(std::string(argv[1]) + "/" + name + ".rgba", std::ios::binary)
                .write(reinterpret_cast<const char*>(drawn.data()), drawn.size());
            std::ofstream(std::string(argv[1]) + "/" + name + ".bc7", std::ios::binary)
                .write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        }
    }
    // Xbox and Switch: every button the PlayStation names, the d-pad and the sticks the game's own.
    for (int pad = 0; pad < PadCount; ++pad) {
        int drawn_count = 0;
        for (int icon = 0; icon < Count; ++icon) {
            Pixels drawn;
            if (!DrawPad(Icon(icon), Pad(pad), drawn)) continue;
            ++drawn_count;
            const auto encoded = Encode(drawn);
            const double error = Error(drawn, encoded);
            worst = std::max(worst, error);
            const std::string name = std::string(pad == PadXbox ? "xbox_" : "switch_") + Names[icon];
            std::printf("%-20s mse %6.1f\n", name.c_str(), error);
            if (argc > 1) {
                std::ofstream(std::string(argv[1]) + "/" + name + ".rgba", std::ios::binary)
                    .write(reinterpret_cast<const char*>(drawn.data()), drawn.size());
                std::ofstream(std::string(argv[1]) + "/" + name + ".bc7", std::ios::binary)
                    .write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            }
        }
        Pixels unused;
        assert(drawn_count == 12 && !DrawPad(L_UDLR, Pad(pad), unused) && !DrawPad(LS, Pad(pad), unused));
    }

    // The tiled block order: for a 32-pixel-wide texture it is the icons' own (Encode), and
    // wider textures continue with the next micro tile to the right.
    for (int by = 0; by < 8; ++by) {
        for (int bx = 0; bx < 8; ++bx) assert(TiledBlockOffset(bx, by, 32) == size_t(Tiled(bx, by)) * 16);
    }
    assert(TiledBlockOffset(8, 0, 1024) == 64 * 16 && TiledBlockOffset(0, 8, 1024) == 32 * 64 * 16);

    // The HUD atlas sprites: 26x26 in their area, transparent around it.
    for (int s = 0; s < HudCount; ++s) {
        const Area& area = HudAreas[s];
        const auto blocks = RenderArea({"space"}, area);
        const int bw = area.w / 4;
        assert(blocks.size() == size_t(bw) * (area.h / 4) * 16);
        auto alpha = [&](int x, int y) {
            int px[16][4];
            DecodeBlock(&blocks[(size_t(y / 4) * bw + x / 4) * 16], px);
            return px[(y % 4) * 4 + x % 4][3];
        };
        assert(alpha(0, 0) == 0 && alpha(area.w - 1, area.h - 1) == 0 && alpha(area.w / 2, 0) == 0);
        assert(alpha(area.w / 2, area.h / 2) > 200);
    }

    // PSNR above ~30 dB: differences no one sees at 32x32 in a menu.
    assert(worst < 65.0);
    std::printf("prompt icons: all tests passed (worst mse %.1f)\n", worst);
    return 0;
}
