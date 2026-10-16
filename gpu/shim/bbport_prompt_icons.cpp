// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: keyboard and mouse, Xbox and Switch key-guide icons (bbport_prompt_icons.h).

#include "bbport_prompt_icons.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

// A private copy of Dear ImGui's stb_truetype (static, so it cannot clash with ImGui's own).
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop

namespace BbPromptIcons {

const char* const Names[Count] = {
    "KG_Cancel", "KG_L1", "KG_L2", "KG_L3", "KG_LS", "KG_L_D", "KG_L_LR", "KG_L_U", "KG_L_UD", "KG_L_UDLR",
    "KG_OK", "KG_R1", "KG_R2", "KG_R3", "KG_RS", "KG_R_L", "KG_R_U", "KG_Start", "KG_TP_L", "KG_TP_R",
};
const uint64_t PlayStationHash[Count] = {
    0x681cd54bf1ebfaeaull, 0xc2e52c29caf88e58ull, 0xa865b38e932d16bbull, 0x22fb8049bd638e46ull,
    0x67620ed3bf36878aull, 0x29ad9450b1d02186ull, 0x8e97d270d7b066cfull, 0xe11b417f55904354ull,
    0xde3b2100c5c414b1ull, 0x3dd2dbcbb8432b36ull, 0xc42aa25bf0baffedull, 0xb3e95215d4a042adull,
    0xc80ea56ca1f84870ull, 0x568eba23a284c2a8ull, 0x0c1d8189222d57a6ull, 0xd8b19c9041c32644ull,
    0x18ba522de454258dull, 0xbbf2bc267e780389ull, 0xe6e94727c7bcf1bbull, 0xdae39773f2aadd03ull,
};
const Area HudAreas[HudCount] = {{660, 120, 28, 32}, {692, 120, 28, 32}, {724, 120, 28, 32}};
const Icon HudIcon[HudCount] = {OK, Cancel, L3};
const uint64_t HudPlayStationHash[HudCount] = {
    0xf5681001af8a6d1full, 0xd5d7e8ebf85a63f6ull, 0x75f17fcc6aff2036ull,
};

namespace {

// The game's icons: a dark plate, tan glyphs.
struct Color { float r, g, b, a; };
constexpr Color kPlate{46, 46, 46, 255};
constexpr Color kRim{30, 30, 30, 255};
constexpr Color kGlyph{195, 161, 113, 255};

stbtt_fontinfo g_font;
bool g_font_ready = false;

/// Premultiplied float canvas.
struct Canvas {
    float px[Size * Size][4]{};
    void Blend(int x, int y, Color c, float coverage) {
        if (x < 0 || y < 0 || x >= Size || y >= Size || coverage <= 0.0f) return;
        const float a = c.a / 255.0f * std::min(coverage, 1.0f);
        float* p = px[y * Size + x];
        p[0] = c.r * a + p[0] * (1 - a);
        p[1] = c.g * a + p[1] * (1 - a);
        p[2] = c.b * a + p[2] * (1 - a);
        p[3] = 255.0f * a + p[3] * (1 - a);
    }
};

struct Clip { float x0 = 0, y0 = 0, x1 = Size, y1 = Size; };

/// Anti-aliased rounded rectangle [x0, x1) x [y0, y1), 4x4 samples per pixel, inside `clip`.
void FillRoundRect(Canvas& c, float x0, float y0, float x1, float y1, float r, Color color, Clip clip = {}) {
    for (int y = int(y0); y < int(std::ceil(y1)); ++y) {
        for (int x = int(x0); x < int(std::ceil(x1)); ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    if (px < x0 || px >= x1 || py < y0 || py >= y1) continue;
                    if (px < clip.x0 || px >= clip.x1 || py < clip.y0 || py >= clip.y1) continue;
                    const float cx = std::clamp(px, x0 + r, x1 - r), cy = std::clamp(py, y0 + r, y1 - r);
                    if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r) ++inside;
                }
            }
            c.Blend(x, y, color, inside / 16.0f);
        }
    }
}

void Line(Canvas& c, float x0, float y0, float x1, float y1, float width, Color color) {
    for (int y = 0; y < Size; ++y) {
        for (int x = 0; x < Size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
                    const float t = len2 > 0 ? std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f) : 0;
                    const float qx = x0 + t * dx - px, qy = y0 + t * dy - py;
                    if (qx * qx + qy * qy <= width * width / 4) ++inside;
                }
            }
            c.Blend(x, y, color, inside / 16.0f);
        }
    }
}

std::vector<int> Codepoints(const std::string& text) {
    std::vector<int> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char b = text[i];
        int cp, n;
        if (b < 0x80) { cp = b; n = 1; }
        else if ((b >> 5) == 6) { cp = b & 0x1f; n = 2; }
        else if ((b >> 4) == 14) { cp = b & 0x0f; n = 3; }
        else { cp = b & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < text.size(); ++k) cp = (cp << 6) | (text[i + k] & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

float TextWidth(const std::vector<int>& cps, float scale) {
    float w = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cps[i], &advance, &lsb);
        w += advance * scale;
        if (i + 1 < cps.size()) w += stbtt_GetCodepointKernAdvance(&g_font, cps[i], cps[i + 1]) * scale;
    }
    return w;
}

/// The text centred on (cx, cy), at the largest pixel height in [min_px, max_px] that fits
/// max_width (the smallest one otherwise).
void Text(Canvas& c, const std::string& text, float cx, float cy, float max_width, float max_px, float min_px,
          Color color) {
    if (!g_font_ready || text.empty()) return;
    const auto cps = Codepoints(text);
    float px = max_px;
    while (px > min_px && TextWidth(cps, stbtt_ScaleForPixelHeight(&g_font, px)) > max_width) px -= 0.5f;
    const float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    // Centre the capital letters: their height is about 0.73 of the ascent in DejaVu Sans.
    const float baseline = cy + ascent * scale * 0.73f / 2;
    // Twice, half a pixel apart: as heavy as the game's bold glyphs.
    for (const float bold : {-0.25f, 0.25f}) {
    float x = cx - TextWidth(cps, scale) / 2 + bold;
    for (size_t i = 0; i < cps.size(); ++i) {
        const float shift_x = x - std::floor(x);
        int w, h, xoff, yoff;
        unsigned char* bitmap = stbtt_GetCodepointBitmapSubpixel(&g_font, scale, scale, shift_x, 0, cps[i], &w, &h,
                                                                &xoff, &yoff);
        for (int by = 0; by < h; ++by) {
            for (int bx = 0; bx < w; ++bx) {
                c.Blend(int(std::floor(x)) + xoff + bx, int(std::round(baseline)) + yoff + by, color,
                        bitmap[by * w + bx] / 255.0f);
            }
        }
        stbtt_FreeBitmap(bitmap, nullptr);
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cps[i], &advance, &lsb);
        x += advance * scale;
        if (i + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance(&g_font, cps[i], cps[i + 1]) * scale;
    }
    }
}

bool IsMouse(const std::string& input) {
    return input == "mouse" || input.find("button") != std::string::npos || input.rfind("mousewheel", 0) == 0;
}

/// A mouse on the plate, with the bound button lit (or the wheel and its direction).
void Mouse(Canvas& c, const std::string& input, float top) {
    const float x0 = 10, x1 = 22, y0 = top, y1 = 28, split = y0 + (y1 - y0) * 0.42f;
    FillRoundRect(c, x0 - 1.4f, y0 - 1.4f, x1 + 1.4f, y1 + 1.4f, 7.4f, kGlyph);
    FillRoundRect(c, x0, y0, x1, y1, 6, kPlate);
    const float mid = (x0 + x1) / 2;
    // The bound button: the body's own outline, cut to that button's half of the top.
    if (input == "leftbutton") FillRoundRect(c, x0, y0, x1, y1, 6, kGlyph, {x0, y0, mid, split});
    if (input == "rightbutton") FillRoundRect(c, x0, y0, x1, y1, 6, kGlyph, {mid, y0, x1, split});
    Line(c, mid, y0, mid, split, 1.3f, kGlyph);
    Line(c, x0, split, x1, split, 1.3f, kGlyph);
    const bool wheel = input == "middlebutton" || input.rfind("mousewheel", 0) == 0;
    FillRoundRect(c, mid - 1.6f, y0 + 2.5f, mid + 1.6f, split - 1.5f, 1.5f, wheel ? kGlyph : kRim);
    if (input == "mousewheelup" || input == "mousewheeldown") {
        Text(c, input == "mousewheelup" ? "\xe2\x96\xb2" : "\xe2\x96\xbc", 27, (y0 + split) / 2, 8, 7, 5, kGlyph);
    }
    if (input == "sidebuttonback" || input == "sidebuttonforward") {
        Text(c, input == "sidebuttonback" ? "4" : "5", mid, (split + y1) / 2 + 1, 8, 9, 6, kGlyph);
    }
}

Pixels Finish(const Canvas& c);

void Plate(Canvas& c) {
    FillRoundRect(c, 1, 1, 31, 31, 7, kRim);
    FillRoundRect(c, 2, 2, 30, 30, 6, kPlate);
}

// ---- BC7 ---------------------------------------------------------------------------------

struct Bits {
    uint8_t out[16]{};
    int at = 0;
    void Put(uint32_t value, int count) {
        for (int i = 0; i < count; ++i, ++at) {
            if ((value >> i) & 1) out[at >> 3] |= uint8_t(1u << (at & 7));
        }
    }
};

constexpr int kWeights2[4] = {0, 21, 43, 64};
constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

int Interp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

using Block = std::array<std::array<int, 4>, 16>;  // RGBA, straight

/// The two pixels farthest apart in the chosen channels: the endpoints of the block's line.
void FarthestPair(const Block& px, int first, int count, const bool* use, std::array<int, 4>& a,
                  std::array<int, 4>& b) {
    int best = -1;
    a = b = px[0];
    for (int i = 0; i < 16; ++i) {
        if (use && !use[i]) continue;
        for (int j = i; j < 16; ++j) {
            if (use && !use[j]) continue;
            int d = 0;
            for (int k = first; k < first + count; ++k) d += (px[i][k] - px[j][k]) * (px[i][k] - px[j][k]);
            if (d > best) { best = d; a = px[i]; b = px[j]; }
        }
    }
}

/// Mode 6: RGBA endpoints of 7 bits plus a p-bit each, 16 shades. Returns the squared error.
long Mode6(const Block& px, uint8_t out[16]) {
    std::array<int, 4> a, b;
    FarthestPair(px, 0, 4, nullptr, a, b);
    long best_err = -1;
    uint8_t best[16];
    for (int pa = 0; pa < 2; ++pa) {
        for (int pb = 0; pb < 2; ++pb) {
            int qa[4], qb[4], ea[4], eb[4];
            for (int k = 0; k < 4; ++k) {
                qa[k] = std::clamp((a[k] - pa + 1) >> 1, 0, 127);
                qb[k] = std::clamp((b[k] - pb + 1) >> 1, 0, 127);
                ea[k] = (qa[k] << 1) | pa;
                eb[k] = (qb[k] << 1) | pb;
            }
            int idx[16];
            long err = 0;
            for (int i = 0; i < 16; ++i) {
                long e_best = -1;
                for (int s = 0; s < 16; ++s) {
                    long e = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int d = Interp(ea[k], eb[k], kWeights4[s]) - px[i][k];
                        e += d * d;
                    }
                    if (e_best < 0 || e < e_best) { e_best = e; idx[i] = s; }
                }
                err += e_best;
            }
            if (best_err >= 0 && err >= best_err) continue;
            best_err = err;
            // Anchor: pixel 0's index has no top bit; swap the endpoints if it would need one.
            Bits bits;
            int pa_out = pa, pb_out = pb;
            int sa[4], sb[4];
            std::memcpy(sa, qa, sizeof sa);
            std::memcpy(sb, qb, sizeof sb);
            if (idx[0] >= 8) {
                std::swap(sa, sb);
                std::swap(pa_out, pb_out);
                for (int& v : idx) v = 15 - v;
            }
            bits.Put(1u << 6, 7);
            for (int k = 0; k < 4; ++k) { bits.Put(sa[k], 7); bits.Put(sb[k], 7); }
            bits.Put(pa_out, 1);
            bits.Put(pb_out, 1);
            bits.Put(idx[0], 3);
            for (int i = 1; i < 16; ++i) bits.Put(idx[i], 4);
            std::memcpy(best, bits.out, 16);
        }
    }
    std::memcpy(out, best, 16);
    return best_err;
}

/// Mode 5: RGB endpoints of 7 bits and alpha endpoints of 8 bits, each with its own 4 shades.
/// Transparent pixels do not pull the colour line. Returns the squared error.
long Mode5(const Block& px, uint8_t out[16]) {
    bool opaque[16];
    bool any = false;
    for (int i = 0; i < 16; ++i) any |= (opaque[i] = px[i][3] > 8);
    std::array<int, 4> a, b;
    FarthestPair(px, 0, 3, any ? opaque : nullptr, a, b);
    int qa[3], qb[3], ea[3], eb[3];
    for (int k = 0; k < 3; ++k) {
        qa[k] = std::clamp((a[k] + 1) >> 1, 0, 127);
        qb[k] = std::clamp((b[k] + 1) >> 1, 0, 127);
        ea[k] = (qa[k] << 1) | (qa[k] >> 6);
        eb[k] = (qb[k] << 1) | (qb[k] >> 6);
    }
    int alo = 255, ahi = 0;
    for (const auto& p : px) { alo = std::min(alo, p[3]); ahi = std::max(ahi, p[3]); }
    int cidx[16], aidx[16];
    long err = 0;
    for (int i = 0; i < 16; ++i) {
        long e_best = -1;
        for (int s = 0; s < 4; ++s) {
            long e = 0;
            for (int k = 0; k < 3; ++k) {
                const int d = Interp(ea[k], eb[k], kWeights2[s]) - px[i][k];
                e += d * d;
            }
            if (e_best < 0 || e < e_best) { e_best = e; cidx[i] = s; }
        }
        // Colour under a transparent pixel is invisible: it only counts as much as it shows.
        err += e_best * px[i][3] / 255;
        long ea_best = -1;
        for (int s = 0; s < 4; ++s) {
            const int d = Interp(alo, ahi, kWeights2[s]) - px[i][3];
            if (ea_best < 0 || d * d < ea_best) { ea_best = d * d; aidx[i] = s; }
        }
        err += ea_best;
    }
    if (cidx[0] >= 2) {
        std::swap(qa, qb);
        for (int& v : cidx) v = 3 - v;
    }
    if (aidx[0] >= 2) {
        std::swap(alo, ahi);
        for (int& v : aidx) v = 3 - v;
    }
    Bits bits;
    bits.Put(1u << 5, 6);
    bits.Put(0, 2);  // no channel rotation
    for (int k = 0; k < 3; ++k) { bits.Put(qa[k], 7); bits.Put(qb[k], 7); }
    bits.Put(alo, 8);
    bits.Put(ahi, 8);
    bits.Put(cidx[0], 1);
    for (int i = 1; i < 16; ++i) bits.Put(cidx[i], 2);
    bits.Put(aidx[0], 1);
    for (int i = 1; i < 16; ++i) bits.Put(aidx[i], 2);
    std::memcpy(out, bits.out, 16);
    return err;
}

/// The game's layout of a 32x32 BC7 texture: one 8x8 micro tile of blocks, Morton order (x in
/// the even bits). Verified against the game's own KG_* icons.
int TiledBlock(int bx, int by) {
    int out = 0;
    for (int bit = 0; bit < 3; ++bit) out |= ((bx >> bit) & 1) << (2 * bit) | ((by >> bit) & 1) << (2 * bit + 1);
    return out;
}

} // namespace

bool SetFont(const unsigned char* ttf) {
    g_font_ready = ttf && stbtt_InitFont(&g_font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    return g_font_ready;
}

std::string KeyLabel(const std::string& input) {
    static const std::map<std::string, std::string> names = {
        {"space", "Space"}, {"lshift", "Shift"}, {"rshift", "Shift"}, {"lctrl", "Ctrl"}, {"rctrl", "Ctrl"},
        {"lalt", "Alt"}, {"ralt", "Alt"}, {"tab", "Tab"}, {"escape", "Esc"}, {"enter", "Enter"},
        {"backspace", "Bksp"}, {"delete", "Del"}, {"insert", "Ins"}, {"home", "Home"}, {"end", "End"},
        {"pgup", "PgUp"}, {"pgdown", "PgDn"}, {"capslock", "Caps"}, {"lmeta", "Win"}, {"rmeta", "Win"},
        {"lwin", "Win"}, {"rwin", "Win"}, {"up", "\xe2\x86\x91"}, {"down", "\xe2\x86\x93"},
        {"left", "\xe2\x86\x90"}, {"right", "\xe2\x86\x92"}, {"grave", "`"}, {"minus", "-"}, {"equals", "="},
        {"lbracket", "["}, {"rbracket", "]"}, {"backslash", "\\"}, {"semicolon", ";"}, {"apostrophe", "'"},
        {"comma", ","}, {"period", "."}, {"slash", "/"}, {"printscreen", "PrtSc"}, {"scrolllock", "ScrLk"},
        {"pausebreak", "Pause"}, {"kpperiod", "Num."}, {"kpcomma", "Num,"}, {"kpslash", "Num/"},
        {"kpasterisk", "Num*"}, {"kpminus", "Num-"}, {"kpplus", "Num+"}, {"kpequals", "Num="},
        {"kpenter", "Enter"},
    };
    if (IsMouse(input)) return {};
    if (auto it = names.find(input); it != names.end()) return it->second;
    if (input.size() == 1) return std::string(1, char(std::toupper(static_cast<unsigned char>(input[0]))));
    if (input.size() >= 2 && input[0] == 'f' && std::isdigit(static_cast<unsigned char>(input[1]))) return "F" + input.substr(1);
    if (input.rfind("kp", 0) == 0) return "Num" + input.substr(2);
    return input;
}

Pixels Draw(const Inputs& inputs) {
    Canvas c;
    Plate(c);
    if (inputs.empty()) {
        Text(c, "\xe2\x80\x93", 16, 16, 24, 18, 10, kGlyph);
    } else if (inputs.size() == 1) {
        // A combination: its last input big, the modifiers small above it.
        std::string mods, main = inputs[0];
        if (const size_t plus = main.rfind('+'); plus != std::string::npos) {
            for (size_t start = 0; start < plus;) {
                const size_t end = std::min(main.find('+', start), plus);
                mods += (mods.empty() ? "" : "+") + KeyLabel(main.substr(start, end - start));
                start = end + 1;
            }
            main = main.substr(plus + 1);
        }
        const float top = mods.empty() ? 4.5f : 11.0f;
        if (!mods.empty()) Text(c, mods, 16, 7.5f, 25, 8, 5.5f, kGlyph);
        if (IsMouse(main)) Mouse(c, main, top);
        else Text(c, KeyLabel(main), 16, mods.empty() ? 16 : 20, 25, mods.empty() ? 17 : 13, 7, kGlyph);
    } else if (std::all_of(inputs.begin(), inputs.end(), [](const std::string& input) {
                   return input == "up" || input == "down" || input == "left" || input == "right";
               })) {
        // Arrow keys for d-pad directions: each where it points, as on the game's d-pad icons.
        for (const auto& input : inputs) {
            const float x = input == "left" ? 9.5f : input == "right" ? 22.5f : 16;
            const float y = input == "up" ? 9.5f : input == "down" ? 22.5f : 16;
            Text(c, KeyLabel(input), x, y, 12, 12, 8, kGlyph);
        }
    } else {
        // A stick or a set of d-pad directions on other keys: side by side ("WASD", "IK").
        std::string label;
        bool mouse = false;
        for (const auto& input : inputs) {
            if (IsMouse(input)) { mouse = true; continue; }
            label += KeyLabel(input.substr(input.rfind('+') == std::string::npos ? 0 : input.rfind('+') + 1));
        }
        if (mouse && label.empty()) Mouse(c, "mouse", 4.5f);
        else Text(c, label, 16, 16, 26, 14, 6, kGlyph);
    }
    return Finish(c);
}

namespace {

// The face buttons' letters: Xbox's own colours, a little darker as the game's PlayStation
// symbols are; the Switch's are all one colour.
constexpr Color kXboxA{96, 168, 58, 255}, kXboxB{206, 54, 48, 255}, kXboxX{48, 112, 208, 255},
                kXboxY{222, 176, 38, 255};

void Round(Canvas& c) {
    FillRoundRect(c, 1, 1, 31, 31, 15, kRim);
    FillRoundRect(c, 2, 2, 30, 30, 14, kPlate);
}

/// A face button: the letter on the round plate, as the game's Cross and Circle are drawn.
void Face(Canvas& c, const char* letter, Color color) {
    Round(c);
    Text(c, letter, 16, 16, 20, 19, 12, color);
}

/// A shoulder button or trigger: its name on the plate, as the game's L1 and R2.
void Shoulder(Canvas& c, const char* name) {
    FillRoundRect(c, 1, 5, 31, 28, 5, kRim);
    FillRoundRect(c, 2, 6, 30, 27, 4, kPlate);
    Text(c, name, 16, 16.5f, 26, 19, 9, kGlyph);
}

} // namespace

bool DrawPad(Icon icon, Pad pad, Pixels& out) {
    const bool xbox = pad == PadXbox;
    Canvas c;
    switch (icon) {
    case Cancel: Face(c, xbox ? "A" : "B", xbox ? kXboxA : kGlyph); break;     // Cross
    case OK: Face(c, xbox ? "B" : "A", xbox ? kXboxB : kGlyph); break;         // Circle
    case R_L: Face(c, xbox ? "X" : "Y", xbox ? kXboxX : kGlyph); break;        // Square
    case R_U: Face(c, xbox ? "Y" : "X", xbox ? kXboxY : kGlyph); break;        // Triangle
    case L1: Shoulder(c, xbox ? "LB" : "L"); break;
    case R1: Shoulder(c, xbox ? "RB" : "R"); break;
    case L2: Shoulder(c, xbox ? "LT" : "ZL"); break;
    case R2: Shoulder(c, xbox ? "RT" : "ZR"); break;
    case L3: case R3:
        // A stick pressed: the game's own stick icon is the letter alone.
        Round(c);
        Text(c, icon == L3 ? "LS" : "RS", 16, 16, 22, 15, 9, kGlyph);
        break;
    case Start:
        Round(c);
        if (xbox) {
            // Menu: three lines.
            for (const float y : {11.0f, 16.0f, 21.0f}) Line(c, 10.5f, y, 21.5f, y, 2.2f, kGlyph);
        } else {
            Line(c, 9.5f, 16, 22.5f, 16, 3, kGlyph);  // +
            Line(c, 16, 9.5f, 16, 22.5f, 3, kGlyph);
        }
        break;
    case TP_L:
        // The touchpad's controller binding is Back: Xbox's View, the Switch's minus.
        Round(c);
        if (xbox) {
            FillRoundRect(c, 12, 9, 23, 18, 1.5f, kGlyph);
            FillRoundRect(c, 13.6f, 10.6f, 21.4f, 16.4f, 0.5f, kPlate);
            FillRoundRect(c, 9, 14, 20, 23, 1.5f, kGlyph);
            FillRoundRect(c, 10.6f, 15.6f, 18.4f, 21.4f, 0.5f, kPlate);
        } else {
            Line(c, 9.5f, 16, 22.5f, 16, 3, kGlyph);
        }
        break;
    default:
        return false;  // the d-pad, the sticks, the touchpad's right half: the game's own
    }
    out = Finish(c);
    return true;
}

namespace {

Pixels Finish(const Canvas& c) {
    Pixels out{};
    for (int i = 0; i < Size * Size; ++i) {
        const float a = c.px[i][3];
        for (int k = 0; k < 3; ++k) {
            // Straight colour; a transparent pixel takes the plate's, so filtering at the rounded
            // edge does not darken toward black.
            const float v = a > 0.5f ? c.px[i][k] * 255.0f / a : (k == 0 ? kPlate.r : k == 1 ? kPlate.g : kPlate.b);
            out[i * 4 + k] = uint8_t(std::clamp(std::lround(v), 0l, 255l));
        }
        out[i * 4 + 3] = uint8_t(std::clamp(std::lround(a), 0l, 255l));
    }
    return out;
}

} // namespace

namespace {
/// The better of modes 5 and 6 for the 4x4 block at (bx, by) of an RGBA image `width` wide.
void EncodeBlock(const uint8_t* rgba, int width, int bx, int by, uint8_t out[16]) {
    Block block;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const uint8_t* p = &rgba[((by * 4 + y) * width + bx * 4 + x) * 4];
            block[y * 4 + x] = {p[0], p[1], p[2], p[3]};
        }
    }
    uint8_t six[16], five[16];
    const long e6 = Mode6(block, six), e5 = Mode5(block, five);
    std::memcpy(out, e6 <= e5 ? six : five, 16);
}
} // namespace

std::array<uint8_t, Bytes> Encode(const Pixels& pixels) {
    std::array<uint8_t, Bytes> out{};
    for (int by = 0; by < Size / 4; ++by) {
        for (int bx = 0; bx < Size / 4; ++bx) EncodeBlock(pixels.data(), Size, bx, by, &out[TiledBlock(bx, by) * 16]);
    }
    return out;
}

size_t TiledBlockOffset(int bx, int by, int width) {
    const int tiles_x = width / 32;  // 8 blocks of 4 pixels per micro tile
    return (size_t(by / 8 * tiles_x + bx / 8) * 64 + size_t(TiledBlock(bx % 8, by % 8))) * 16;
}

std::vector<uint8_t> RenderArea(const Inputs& inputs, const Area& area) {
    return RenderIconArea(Draw(inputs), area);
}

std::vector<uint8_t> RenderIconArea(const Pixels& icon, const Area& area) {
    // The 32x32 icon scaled to the game's 26x26 sprite (each output pixel the alpha-weighted
    // average of what it covers), centred in an otherwise transparent area.
    constexpr int kSprite = 26;
    std::vector<uint8_t> rgba(size_t(area.w) * area.h * 4, 0);
    const int ox = (area.w - kSprite) / 2, oy = (area.h - kSprite) / 2;
    const float step = float(Size) / kSprite;
    for (int y = 0; y < kSprite; ++y) {
        for (int x = 0; x < kSprite; ++x) {
            float sum[4]{}, weight = 0;
            int samples = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx, ++samples) {
                    const int px = std::min(Size - 1, int((x + (sx + 0.5f) / 4) * step));
                    const int py = std::min(Size - 1, int((y + (sy + 0.5f) / 4) * step));
                    const uint8_t* p = &icon[(py * Size + px) * 4];
                    const float a = p[3] / 255.0f;
                    for (int k = 0; k < 3; ++k) sum[k] += p[k] * a;
                    sum[3] += p[3];
                    weight += a;
                }
            }
            uint8_t* out = &rgba[(size_t(oy + y) * area.w + ox + x) * 4];
            for (int k = 0; k < 3; ++k) out[k] = uint8_t(weight > 0 ? std::lround(sum[k] / weight) : kPlate.r);
            out[3] = uint8_t(std::lround(sum[3] / samples));
        }
    }
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (!rgba[i + 3]) { rgba[i] = rgba[i + 1] = rgba[i + 2] = uint8_t(kPlate.r); }
    }
    std::vector<uint8_t> blocks(size_t(area.w / 4) * (area.h / 4) * 16);
    for (int by = 0; by < area.h / 4; ++by) {
        for (int bx = 0; bx < area.w / 4; ++bx) {
            EncodeBlock(rgba.data(), area.w, bx, by, &blocks[(size_t(by) * (area.w / 4) + bx) * 16]);
        }
    }
    return blocks;
}

} // namespace BbPromptIcons
