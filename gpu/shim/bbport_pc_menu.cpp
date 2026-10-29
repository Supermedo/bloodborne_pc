// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: PC Settings in the game's own System menu (game 1.09; scripts/pc_menu.py patches the
// menu and the page opener, adds the PC page to optionsetting.gfx and the texts to the Quit to
// Desktop text layer). Each page is the game's own settings screen built with the game's own
// choice lines, whose values are ints here. A thread applies what changes to the port's settings
// (bbport_settings.h) and saves bbport.ini, as the Insert menu does.
//
// The game calls in twice: the System menu's builder asks for the lines ("bb:o" through the
// browser import, runtime_services.c), and a line's handler opens the PC page, whose builder is
// Build<line> below. The three lines share that one page (pc_menu.py): each builds its own
// lines and shows its own title. Everything else is the game's own code, called with its
// calling convention.

#include "bbport_settings.h"
#include "../bbgpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace {

#define SYSV __attribute__((sysv_abi))

// Image offsets in the 1.09 eboot.
constexpr uint32_t TEXT = 0x1AE8CC0;          // text(out 0x40 bytes, FMG, id)
constexpr uint32_t STRING_ASSIGN = 0x26279A0; // string assign(dst, src, pos, count)
constexpr uint32_t FUNCTION_COPY = 0x1B6ED90; // std::function copy(src, dst buffer) -> callable
constexpr uint32_t ADD_LINE = 0x1B4E2A0;      // System menu line(list, texts, function, out)
constexpr uint32_t OPEN_PAGE = 0x1BB4C70;     // a line's page(out, context, factory)
constexpr uint32_t PAGE = 0x1B20900;          // settings page(a, b, name, builder, adjust, first)
constexpr uint32_t CHOICE_APPEND = 0x1B1C0C0; // choices (up to 32, int values): append(list, entry)
constexpr uint32_t CHOICE_LINE = 0x1B29370;   // choice line(page, texts, value*, list, default*)
constexpr uint32_t ON_OFF = 0x1B2B3B0;        // On / Off choices (two, byte values): make(list)
constexpr uint32_t SWITCH_LINE = 0x1B2A100;   // On / Off line(page, texts, byte*, list, default byte*)
constexpr uint32_t CHILD = 0x1ADB220;         // a movie child's object(out 0x60 bytes, parent, name, ...)
constexpr uint32_t LINE_FUNCTION = 0x533D120; // __func vtable of the System menu lines' handlers
constexpr uint32_t PAGE_FUNCTION = 0x5343880; // __func vtable of the pages' factories
constexpr uint32_t PAGE_CLIP = 0xDF0;         // a page's movie clip (its rows' parent, 0x1B2BA20)
// pc_menu.py's page name table (in the code segment's last page, after its magic): image
// offsets of the game's six names and the PC page's, made addresses when the game is loaded.
constexpr uint32_t CAVE = 0x50D9B00, CAVE_END = 0x50DA000;
constexpr char TABLE_MAGIC[8] = {'b', 'b', 'P', 'C', 'p', 'a', 'g', 0};
constexpr int TABLE_NAMES = 6 + 1;
constexpr int NAMES = 200, HELP = 201;        // menu.msgbnd.dcx FMGs: item names, one-line help
constexpr int ON = 40300, OFF = 40301;        // the game's own On / Off

// Text ids (scripts/pc_menu.py writes the same): the pages (System line, title, help), each
// line (name and help), then each line's choice labels.
constexpr int PAGE_TEXT = 119001, LINE_BASE = 119010;
// The opener's Defaults button and its question (the game's), Restart Game on the PC page.
constexpr int DEFAULTS = 40330, DEFAULTS_QUESTION = 40300, RESTART = 119009;
constexpr int UPSCALER_TEXT = 119100, PRESET_TEXT = 119110, SHARPNESS_TEXT = 119120,
              FRAME_GEN_TEXT = 119140, MULTIPLIER_TEXT = 119150, OUTPUT_TEXT = 119160,
              FRAME_RATE_TEXT = 119180, FRAME_CAP_TEXT = 119190, FOV_TEXT = 119200,
              LOD_TEXT = 119220, HUD_TEXT = 119230, PROMPTS_TEXT = 119250, RESTART_TEXT = 119260;

enum Line : int {
    Upscaler, Quality, Sharpness, FrameGen, Multiplier, Output, FrameRate, FrameCap,
    ModelDetail, AmbientOcclusion, Shadows, Reflections, GameAa, MotionBlur, DepthOfField,
    Chromatic, Fov, CameraDistance, CameraRotation, HudPosition, ShowFps, Prompts, Mute,
    SkipIntro, Restart, LineCount
};
// The PC page (pc_menu.py PAGE, its name in optionsetting.gfx), its titles (TITLE_NAME), and
// each System line's lines on it; the ninth row is the opener's button, Restart Game here.
constexpr int PageCount = 3, PageLines = 8;
constexpr const char* PageName = "PcSettings";
constexpr const char* TitleNames[PageCount] = {"PcTitle_0", "PcTitle_1", "PcTitle_2"};
constexpr Line Pages[PageCount][PageLines] = {
    {Upscaler, Quality, Sharpness, FrameGen, Multiplier, Output, FrameRate, FrameCap},
    {ModelDetail, AmbientOcclusion, Shadows, Reflections, GameAa, MotionBlur, DepthOfField, Chromatic},
    {Fov, CameraDistance, CameraRotation, HudPosition, ShowFps, Prompts, Mute, SkipIntro},
};
// Lines switched by the community patches at start (patches.py): their bbport.ini keys.
struct EffectLine {
    Line line;
    const char* key;
    bool inverted; // "Auto camera rotation" shows the opposite of tweak_no_camera_rotation
};
constexpr EffectLine EffectLines[] = {
    {Chromatic, "effect_chromatic_aberration", false}, {DepthOfField, "effect_dof", false},
    {MotionBlur, "effect_motion_blur", false},         {AmbientOcclusion, "effect_ssao", false},
    {GameAa, "effect_game_aa", false},                 {Shadows, "effect_dynamic_shadows", false},
    {Reflections, "effect_ssr", false},                {CameraDistance, "tweak_camera_distance", false},
    {CameraRotation, "tweak_no_camera_rotation", true}, {SkipIntro, "skip_intro", false},
};
// Sharpness choices: 0 sharpening off, then these strengths.
constexpr float SharpnessSteps[] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                                    0.9f, 1.0f, 1.5f, 2.0f};
constexpr int SharpnessCount = int(sizeof(SharpnessSteps) / sizeof(*SharpnessSteps));
constexpr int FovSteps[] = {30, 35, 40, 43, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90};
constexpr int FovCount = int(sizeof(FovSteps) / sizeof(*FovSteps));

unsigned char* image;
std::once_flag applier_started;
// The PC page being built (until the opener has its button's texts) and the one open: the
// game's page objects, for the Defaults button's texts and Yes. Opening a game page clears it.
std::atomic<void*> building_page{nullptr}, pc_page{nullptr};
std::atomic<int> building_texts{0};

void RestartSoon() {
    BbSettings::Save();
    std::printf("PC Settings: restarting the game to apply the changes\n");
    std::fflush(stdout);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        runtime_restart();
    }).detach();
}
// The lines' values (the game's lines write them) and their defaults ("Default" on the page):
// ints for choice lines, bytes for On / Off lines.
int32_t values[LineCount], defaults[LineCount], applied[LineCount];
uint8_t switches[LineCount], switch_defaults[LineCount];

bool IsSwitch(Line line) {
    switch (line) {
    case AmbientOcclusion: case Shadows: case Reflections: case GameAa: case MotionBlur:
    case DepthOfField: case Chromatic: case CameraDistance: case CameraRotation: case ShowFps:
    case Mute: case SkipIntro:
        return true;
    default:
        return false;
    }
}

int32_t Value(int line) {
    return IsSwitch(Line(line)) ? __atomic_load_n(&switches[line], __ATOMIC_RELAXED)
                                : __atomic_load_n(&values[line], __ATOMIC_RELAXED);
}
void SetValue(int line, int32_t v) {
    __atomic_store_n(&values[line], v, __ATOMIC_RELAXED);
    __atomic_store_n(&switches[line], uint8_t(v != 0), __ATOMIC_RELAXED);
}

template <typename R, typename... A>
R Guest(uint32_t offset, A... args) {
    using F = R(SYSV*)(A...);
    return reinterpret_cast<F>(image + offset)(args...);
}

uint64_t Read64(const unsigned char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void Write64(unsigned char* p, uint64_t v) { std::memcpy(p, &v, 8); }

// The game's strings (0x30 bytes): +8 the characters or their heap block, +0x20 capacity
// (heap from 8), +0x28 the allocator, which frees through its vtable's slot 0x70.
void FreeString(unsigned char* s) {
    if (Read64(s + 0x20) < 8) return;
    auto* allocator = reinterpret_cast<void*>(Read64(s + 0x28));
    auto* vtable = *reinterpret_cast<uintptr_t* const*>(allocator);
    reinterpret_cast<void(SYSV*)(void*, void*)>(vtable[0x70 / 8])(allocator,
                                                                    reinterpret_cast<void*>(Read64(s + 8)));
}
// A text (0x40 bytes: +0 its id, +8 its string, +0x38 a flag).
void LookUp(unsigned char* text, int fmg, int id) { Guest<void>(TEXT, text, fmg, id); }

// std::function, libc++ (0x28 bytes used): its own 0x20-byte buffer, then the callable's
// address. A plain function pointer sits in the buffer: {__func vtable, function}.
void DestroyFunction(unsigned char* function) {
    const uint64_t callable = Read64(function + 0x20);
    if (!callable) return;
    auto* vtable = *reinterpret_cast<uintptr_t* const*>(callable);
    reinterpret_cast<void(SYSV*)(uint64_t, int)>(vtable[0x20 / 8])(
        callable, callable != reinterpret_cast<uint64_t>(function));
}

// Up to 32 choices with int values (as the Language lines'): 0x48-byte entries, count at +0x908.
struct alignas(16) Choices {
    unsigned char raw[0x910];
    Choices() { std::memset(raw, 0, sizeof(raw)); }
    ~Choices() {
        const uint64_t count = Read64(raw + 0x908);
        for (uint64_t i = 0; i < count && i < 32; ++i) FreeString(raw + i * 0x48 + 0x10);
    }
    void Add(int32_t value, int id, int fmg = NAMES) {
        if (Read64(raw + 0x908) >= 32) return;
        alignas(16) unsigned char text[0x40];
        LookUp(text, fmg, id);
        alignas(16) unsigned char entry[0x48] = {};
        std::memcpy(entry, &value, 4);
        std::memcpy(entry + 8, text, 8);
        std::memcpy(entry + 0x38, text + 0x30, 8); // the string's allocator
        Write64(entry + 0x30, 7);                   // empty, in place
        Guest<void*>(STRING_ASSIGN, entry + 0x10, text + 8, uint64_t(0), ~uint64_t(0));
        entry[0x40] = text[0x38];
        Guest<void>(CHOICE_APPEND, raw, entry);
        FreeString(entry + 0x10);
        FreeString(text + 8);
    }
    void OnOff() {
        Add(1, ON);
        Add(0, OFF);
    }
};

int EffectIndex(const char* key) {
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (!std::strcmp(BbSettings::Effects[e].key, key)) return e;
    }
    return -1;
}

int NearestSharpness(float value) {
    int best = 1;
    for (int i = 2; i < SharpnessCount; ++i) {
        if (std::fabs(SharpnessSteps[i] - value) < std::fabs(SharpnessSteps[best] - value)) best = i;
    }
    return best;
}
int NearestFov(int value) {
    int best = 0;
    for (int i = 1; i < FovCount; ++i) {
        if (std::abs(FovSteps[i] - value) < std::abs(FovSteps[best] - value)) best = i;
    }
    return FovSteps[best];
}

// The port's settings as the lines show them.
void ReadSettings(int32_t* out) {
    const auto& s = BbSettings::Get();
    out[Upscaler] = s.upscaler;
    out[Quality] = s.preset;
    out[Sharpness] = s.sharpen ? NearestSharpness(s.sharpness) : 0;
    out[FrameGen] = s.frame_gen;
    out[Multiplier] = std::clamp(s.frame_gen_multiplier.load(), 2, 4);
    out[Output] = s.output_res;
    out[FrameRate] = s.fps_mode;
    out[FrameCap] = s.frame_cap;
    out[Fov] = NearestFov(s.fov);
    out[ModelDetail] = s.model_lod;
    out[HudPosition] = s.hud_position / 10 * 10;
    for (const auto& e : EffectLines) {
        const int index = EffectIndex(e.key);
        out[e.line] = index >= 0 && (s.effects[index] != e.inverted);
    }
    out[ShowFps] = s.show_fps;
    out[Prompts] = s.button_prompts;
    out[Mute] = s.mute_unfocused;
    out[Restart] = 0;
}

void DefaultSettings(int32_t* out) {
    out[Upscaler] = BbSettings::UpscalerFsr3;
    out[Quality] = BbSettings::Quality;
    out[Sharpness] = 5; // 0.5
    out[FrameGen] = BbSettings::FrameGenOff;
    out[Multiplier] = 2;
    out[Output] = BbSettings::OutputDefault;
    out[FrameRate] = BbSettings::FpsUncap;
    out[FrameCap] = BbSettings::FrameCapAuto;
    out[Fov] = BbSettings::FovDefault;
    out[ModelDetail] = 0;
    out[HudPosition] = 100;
    for (const auto& e : EffectLines) {
        const int index = EffectIndex(e.key);
        out[e.line] = index >= 0 && (BbSettings::Effects[index].default_on != e.inverted);
    }
    out[ShowFps] = 0;
    out[Prompts] = BbSettings::PromptsAuto;
    out[Mute] = 0;
    out[Restart] = 0;
}

// One changed line to the port's settings. Returns true when bbport.ini should be written.
bool Apply(Line line, int32_t v) {
    auto& s = BbSettings::Get();
    switch (line) {
    case Upscaler: s.upscaler = std::clamp(v, 0, BbSettings::UpscalerCount - 1); return true;
    case Quality: s.preset = std::clamp(v, 0, BbSettings::PresetCount - 1); return true;
    case Sharpness:
        s.sharpen = v > 0;
        if (v > 0) s.sharpness = SharpnessSteps[std::clamp(v, 1, SharpnessCount - 1)];
        return true;
    case FrameGen: BbSettings::SelectFrameGen(std::clamp(v, 0, BbSettings::FrameGenCount - 1)); return true;
    case Multiplier: s.frame_gen_multiplier = std::clamp(v, 2, 4); return true;
    case Output: s.output_res = std::clamp(v, 0, BbSettings::OutputCount - 1); return true;
    case FrameRate: s.fps_mode = std::clamp(v, 0, BbSettings::FpsModeCount - 1); return true;
    case FrameCap: s.frame_cap = v; return true;
    case Fov: s.fov = std::clamp(v, BbSettings::FovMin, BbSettings::FovMax); return true;
    case ModelDetail: s.model_lod = std::clamp(v, -2, 2); return true;
    case HudPosition: s.hud_position = std::clamp(v, 0, 100); return true;
    case ShowFps: s.show_fps = v != 0; return true;
    case Prompts: s.button_prompts = std::clamp(v, 0, BbSettings::PromptsCount - 1); return true;
    case Mute: s.mute_unfocused = v != 0; return true;
    case Restart:
        if (v) {
            SetValue(Restart, 0);
            RestartSoon();
        }
        return false;
    default:
        for (const auto& e : EffectLines) {
            if (e.line == line) {
                const int index = EffectIndex(e.key);
                if (index >= 0) s.effects[index] = (v != 0) != e.inverted;
                return true;
            }
        }
        return false;
    }
}

// Applies what the pages change, then saves bbport.ini once the changes stop for a moment. A
// dropdown writes its value as its cursor moves, before it is confirmed (and back on cancel):
// a value is applied once it has stayed for a while, else browsing the frame generation list
// created and destroyed providers frame by frame and crashed the driver.
void Applier() {
    using Clock = std::chrono::steady_clock;
    constexpr auto Settle = std::chrono::milliseconds(800);
    bool dirty = false;
    auto last_change = Clock::now();
    int32_t seen[LineCount];
    Clock::time_point seen_since[LineCount];
    for (int line = 0; line < LineCount; ++line) {
        seen[line] = applied[line];
        seen_since[line] = Clock::now();
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        for (int line = 0; line < LineCount; ++line) {
            const int32_t v = Value(line);
            if (v != seen[line]) {
                seen[line] = v;
                seen_since[line] = now;
            }
            if (v == applied[line] || now - seen_since[line] < Settle) continue;
            applied[line] = v;
            if (Apply(Line(line), v)) {
                dirty = true;
                last_change = now;
            }
        }
        if (dirty && Clock::now() - last_change > std::chrono::milliseconds(400)) {
            dirty = false;
            BbSettings::Save();
        }
    }
}

// A line's choices, as the settings are now.
void LineChoices(Line line, const int32_t* now, Choices& c) {
    const auto& s = BbSettings::Get();
    switch (line) {
    case Upscaler:
        for (int u = 0; u < BbSettings::UpscalerCount; ++u) {
            const bool supported = u == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                                 : u == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                 : u == BbSettings::UpscalerDlss ? s.dlss_supported.load() : true;
            if (supported || u == now[Upscaler]) c.Add(u, UPSCALER_TEXT + u);
        }
        break;
    case Quality:
        for (int p = 0; p < BbSettings::PresetCount; ++p) c.Add(p, PRESET_TEXT + p);
        break;
    case Sharpness:
        for (int i = 0; i < SharpnessCount; ++i) c.Add(i, SHARPNESS_TEXT + i);
        break;
    case FrameGen:
        for (int f = 0; f < BbSettings::FrameGenCount; ++f) {
            const bool supported = f == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier > 1
                                 : f == BbSettings::FrameGenFsr401 ? s.fg401_supported.load() : true;
            if (supported || f == now[FrameGen]) c.Add(f, FRAME_GEN_TEXT + f);
        }
        break;
    case Multiplier: {
        const int most = std::min(4, std::max({2, s.dlssg_max_multiplier.load(), now[Multiplier]}));
        for (int m = 2; m <= most; ++m) c.Add(m, MULTIPLIER_TEXT + m - 2);
        break;
    }
    case Output:
        for (int o = 0; o < BbSettings::OutputCount; ++o) c.Add(o, OUTPUT_TEXT + o);
        break;
    case FrameRate:
        for (int f = 0; f < BbSettings::FpsModeCount; ++f) c.Add(f, FRAME_RATE_TEXT + f);
        break;
    case FrameCap:
        for (int i = 0; i < BbSettings::FrameCapCount; ++i) c.Add(BbSettings::FrameCaps[i], FRAME_CAP_TEXT + i);
        break;
    case Fov:
        for (int i = 0; i < FovCount; ++i) c.Add(FovSteps[i], FOV_TEXT + i);
        break;
    case ModelDetail:
        for (int lod = -2; lod <= 2; ++lod) c.Add(lod, LOD_TEXT + lod + 2);
        break;
    case HudPosition:
        for (int i = 0; i <= 10; ++i) c.Add(i * 10, HUD_TEXT + i);
        break;
    case Prompts:
        for (int p = 0; p < BbSettings::PromptsCount; ++p) c.Add(p, PROMPTS_TEXT + p);
        break;
    case Restart:
        c.Add(0, RESTART_TEXT);
        c.Add(1, RESTART_TEXT + 1);
        break;
    default:
        c.OnOff();
        break;
    }
}

// Shows or hides the movie clip `name` under `parent` (a child object: slot 1 is its visibility),
// then lets the child object go as the game does (the managed value it holds).
void SetVisible(void* parent, const char* name, bool visible) {
    alignas(16) unsigned char child[0x60] = {};
    reinterpret_cast<void(SYSV*)(void*, void*, const char*, ...)>(image + CHILD)(child, parent, name);
    auto* vtable = *reinterpret_cast<uintptr_t* const*>(child);
    if (!vtable) return;
    reinterpret_cast<void(SYSV*)(void*, int)>(vtable[1])(child, visible);
    if (child[0x48] & 0x40) {
        auto* value = reinterpret_cast<void*>(Read64(child + 0x40));
        auto* value_vtable = *reinterpret_cast<uintptr_t* const*>(value);
        reinterpret_cast<void(SYSV*)(void*, void*, uint64_t)>(value_vtable[3])(value, child + 0x30,
                                                                                  Read64(child + 0x50));
    }
}

// A line's page builder (the game calls it as a member function: page, settings copy).
template <int P>
SYSV void Build(void* page, void*) {
    building_page = page;
    building_texts = 0;
    std::call_once(applier_started, [] {
        ReadSettings(applied);
        for (int line = 0; line < LineCount; ++line) SetValue(line, applied[line]);
        DefaultSettings(defaults);
        for (int line = 0; line < LineCount; ++line) switch_defaults[line] = uint8_t(defaults[line] != 0);
        std::thread(Applier).detach();
    });
    // The page shows the settings as they are now (the Insert menu may have changed them).
    int32_t now[LineCount];
    ReadSettings(now);
    for (int line = 0; line < LineCount; ++line) {
        SetValue(line, now[line]);
        applied[line] = now[line];
    }
    // Its own title of the three.
    auto* clip = static_cast<unsigned char*>(page) + PAGE_CLIP;
    for (int p = 0; p < PageCount; ++p) SetVisible(clip, TitleNames[p], p == P);
    for (Line line : Pages[P]) {
        alignas(16) unsigned char texts[0x80];
        LookUp(texts, NAMES, LINE_BASE + line);
        LookUp(texts + 0x40, HELP, LINE_BASE + line);
        if (IsSwitch(line)) {
            // The game's own On / Off switch (as Camera Auto-Adjust): two entries, count at +0x98.
            alignas(16) unsigned char choices[0xA0] = {};
            Guest<void*>(ON_OFF, choices);
            Guest<void>(SWITCH_LINE, page, texts, &switches[line], choices, &switch_defaults[line]);
            for (uint64_t i = 0; i < Read64(choices + 0x98) && i < 2; ++i) FreeString(choices + i * 0x48 + 0x10);
        } else {
            Choices c;
            LineChoices(line, now, c);
            Guest<void>(CHOICE_LINE, page, texts, &values[line], c.raw, &defaults[line]);
        }
        FreeString(texts + 8);
        FreeString(texts + 0x48);
    }
}

// A line's page factory (std::function<...(a, b)>, as the Environment line's).
template <int P>
SYSV void* Factory(void* a, void* b) {
    return Guest<void*>(PAGE, a, b, PageName, reinterpret_cast<uintptr_t>(&Build<P>), uintptr_t(0), 0);
}

// A System line's handler (as Environment's): opens the PC page with its lines.
template <int P>
SYSV void* Handler(void* out, void* context) {
    alignas(16) unsigned char factory[0x30] = {};
    Write64(factory, reinterpret_cast<uint64_t>(image + PAGE_FUNCTION));
    Write64(factory + 8, reinterpret_cast<uint64_t>(&Factory<P>));
    Write64(factory + 0x20, reinterpret_cast<uint64_t>(factory));
    Guest<void*>(OPEN_PAGE, out, context, factory);
    DestroyFunction(factory);
    return out;
}

constexpr void* (SYSV* Handlers[PageCount])(void*, void*) = {&Handler<0>, &Handler<1>, &Handler<2>};

void* AddLine(void* list, int text, void* (SYSV* handler)(void*, void*)) {
    alignas(16) unsigned char texts[0x80];
    LookUp(texts, NAMES, text);
    LookUp(texts + 0x40, HELP, text);
    alignas(16) unsigned char source[0x30] = {};
    Write64(source, reinterpret_cast<uint64_t>(image + LINE_FUNCTION));
    Write64(source + 8, reinterpret_cast<uint64_t>(handler));
    Write64(source + 0x20, reinterpret_cast<uint64_t>(source));
    alignas(16) unsigned char function[0x30] = {};
    Write64(function + 0x20, reinterpret_cast<uint64_t>(Guest<void*>(FUNCTION_COPY, source, function)));
    uint64_t out = 0;
    void* lines = Guest<void*>(ADD_LINE, list, texts, function, &out);
    DestroyFunction(function);
    DestroyFunction(source);
    FreeString(texts + 8);
    FreeString(texts + 0x48);
    return lines;
}

} // namespace

extern "C" void bbgpu_pc_menu_set_image(unsigned char* base) { image = base; }

// After the patches, before the code is protected: pc_menu.py's page name table gets addresses.
extern "C" void bbgpu_pc_menu_prepare(unsigned char* base) {
    for (uint32_t at = CAVE; at + 8 + TABLE_NAMES * 8 <= CAVE_END; at += 8) {
        if (std::memcmp(base + at, TABLE_MAGIC, 8) != 0) continue;
        unsigned char* table = base + at + 8;
        for (int i = 0; i < TABLE_NAMES; ++i) {
            Write64(table + i * 8, reinterpret_cast<uint64_t>(base) + Read64(table + i * 8));
        }
        std::printf("PC Settings: %d settings pages\n", TABLE_NAMES);
        return;
    }
}

// "bb:h" (runtime_services.c): the menu movie is set up and has hidden the game's pages (each
// child's slot 1 with false); `movie` is its root. The PC page is hidden the same way, else it
// showed unset over the System menu until a page was opened.
extern "C" int bbgpu_pc_menu_hide(void* movie) {
    if (image && movie) SetVisible(movie, PageName, false);
    return 0;
}

// The opener's Defaults button ("bb:l" for its name, then its help, with the page being opened):
// Restart Game on a PC page. Any other page being opened means the PC page is gone.
extern "C" int bbgpu_pc_menu_button_text(void* page) {
    if (page && page == building_page.load()) {
        pc_page = page;
        if (++building_texts >= 2) building_page = nullptr;
        return RESTART;
    }
    pc_page = nullptr;
    return DEFAULTS;
}

// The button's question ("bb:r", with the page asking).
extern "C" int bbgpu_pc_menu_question(void* page) {
    return page && page == pc_page.load() ? RESTART : DEFAULTS_QUESTION;
}

// Its Yes ("bb:y"): 1 when the port has taken it (a PC page: restart), else the game restores
// the page's defaults.
extern "C" int bbgpu_pc_menu_yes(void* page) {
    if (!page || page != pc_page.load()) return 0;
    RestartSoon();
    return 1;
}

// "bb:o" (runtime_services.c): `list` points at the System menu's line list, which each new
// line may replace.
extern "C" int bbgpu_pc_menu_add_line(void* list) {
    if (!image || !list) return 0;
    void** lines = static_cast<void**>(list);
    for (int p = 0; p < PageCount; ++p) *lines = AddLine(*lines, PAGE_TEXT + p, Handlers[p]);
    return 0;
}
