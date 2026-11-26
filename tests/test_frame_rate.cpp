// Test of gpu/shim/bbport_frame_rate.h: the values for a frame,
// applied every frame, give what the game gives at 30 FPS in the same time; the main loop's step is
// read from its instructions in the game's form and the patches' two forms (other bytes: unknown)
// and computed as they compute it; an operand moves to the live value only at the expected bytes;
// the fixed step's detour stores the live value, read at each run, and keeps the other registers.
//
#include "gpu/shim/bbport_frame_rate.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define NOMINMAX // the build compiles the GPU library so too
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int failed = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                       \
            ++failed;                                                                              \
        }                                                                                          \
    } while (0)

using namespace BbFrameRate;

static bool Near(double a, double b, double eps = 2e-6) {
    return std::fabs(a - b) <= eps * std::max(1.0, std::fabs(b));
}

static unsigned char* Executable(std::size_t size) {
#ifdef _WIN32
    return static_cast<unsigned char*>(
        VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
#endif
}

static std::uint32_t Bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

int main() {
    // At 30 FPS the values are the game's own constants, to the bit.
    const Values v30 = ValuesFor(Frame30);
    const float own[] = {1.0f / 30, 1.0f / 30, 0.1f, 0.1f, 0.8f, 1.2f};
    static_assert(sizeof(own) == sizeof(Values));
    CHECK(std::memcmp(&v30, own, sizeof(own)) == 0);
    CHECK(Near(v30.dt, 1.0 / 30) && Near(v30.approach30, 1.0 / 30));
    CHECK(Near(v30.approach10, 0.1) && Near(v30.step, 0.1) && Near(v30.decay08, 0.8));
    CHECK(Near(v30.grow12, 1.2));

    // n frames of 1/(30 n) s do what one 30 FPS frame does.
    for (const int n : {2, 3, 4, 8, 16}) {
        const Values v = ValuesFor(1.0f / (30.0f * float(n)));
        CHECK(Near(v.dt * n, 1.0 / 30));
        CHECK(Near(std::pow(1.0 - v.approach30, n), 1.0 - 1.0 / 30));
        CHECK(Near(std::pow(1.0 - v.approach10, n), 0.9));
        CHECK(Near(v.step * n, 0.1));
        CHECK(Near(std::pow(double(v.decay08), n), 0.8));
        CHECK(Near(std::pow(double(v.grow12), n), 1.2));
    }

    // The main loop's step, read from its instructions (an image as large as the game's up to there).
    {
        std::vector<unsigned char> img(MainStepEnd + 0x1000, 0xcc);
        unsigned char* at = img.data() + MainStepAt;
        const auto fixed_head = [&](float fixed) {
            const unsigned char head[] = {0xc7, 0x45, 0xc8};
            std::memcpy(at, head, 3);
            std::memcpy(at + 3, &fixed, 4);
        };
        // The game's own: a fixed 1/30 s (1/60 s with a "no deltatime" patch).
        const unsigned char game[] = {0x48, 0x8d, 0x05, 0xcc, 0x76, 0x52, 0x03,
                                      0x48, 0x83, 0x38, 0x00, 0x75, 0x21};
        fixed_head(1.0f / 30);
        std::memcpy(at + 7, game, sizeof(game));
        MainStep m = ReadMainStep(img.data(), img.size());
        CHECK(m.form == MainStep::Fixed && Near(m.fixed, 1.0 / 30));
        CHECK(Near(StepFor(m, 1.0f / 120, 1.0f / 240), 1.0 / 30));
        fixed_head(1.0f / 60);
        m = ReadMainStep(img.data(), img.size());
        CHECK(m.form == MainStep::Fixed && Near(StepFor(m, 1.0f / 45, 1.0f / 60), 1.0 / 60));
        // The built-in presets' form: the fixed time at a 1/30 s target, else the measurement
        // between the target and 1/30 s.
        const unsigned char unless30[] = {
            0x81, 0x7b, 0x18, 0x89, 0x88, 0x08, 0x3d, 0x74, 0x1e, 0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d,
            0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00, 0xf3, 0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d,
            0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x45, 0xc8, 0x48, 0x8d, 0x05, 0xa5, 0x76, 0x52, 0x03};
        fixed_head(1.0f / 60);
        std::memcpy(at + 7, unless30, sizeof(unless30));
        m = ReadMainStep(img.data(), img.size());
        CHECK(m.form == MainStep::ClampUnlessTarget30);
        CHECK(Near(StepFor(m, 1.0f / 120, 1.0f / 240), 1.0 / 120));
        CHECK(Near(StepFor(m, 1.0f / 500, 1.0f / 240), 1.0 / 240));
        CHECK(Near(StepFor(m, 0.1f, 1.0f / 240), 1.0 / 30));
        CHECK(Near(StepFor(m, 1.0f / 120, 1.0f / 30), 1.0 / 60)); // the fixed time at 1/30 s
        CHECK(Near(StepFor(m, NAN, 1.0f / 240), 1.0 / 240));      // maxss: the target
        CHECK(Near(StepFor(m, 1.0f / 120, NAN), 1.0 / 30)); // maxss: the NaN, minss: the bound
        // The patches folder's form: the measurement between the target and a constant bound
        // (Uncap FPS++ there sets the target to 0).
        const unsigned char clamp_head[] = {0xf3, 0x0f, 0x10, 0x83, 0x64, 0x02, 0x00, 0x00, 0xf3,
                                            0x0f, 0x5f, 0x43, 0x18, 0xf3, 0x0f, 0x5d, 0x05};
        const unsigned char clamp_mid[] = {0xf3, 0x0f, 0x11, 0x45, 0xc8, 0xf3, 0x0f, 0x11, 0x05};
        const unsigned char clamp_tail[] = {0x48, 0x8d, 0x05, 0xaa, 0x76, 0x52, 0x03,
                                            0x90, 0x90, 0x90, 0x90, 0x90};
        unsigned char* code = at + 7;
        std::memcpy(code, clamp_head, sizeof(clamp_head));
        const std::size_t bound_at = MainStepEnd + 0x100;
        const std::int32_t disp = std::int32_t(bound_at - (MainStepAt + 7 + 21));
        std::memcpy(code + 17, &disp, 4);
        std::memcpy(code + 21, clamp_mid, sizeof(clamp_mid));
        std::memcpy(code + 34, clamp_tail, sizeof(clamp_tail));
        const float bound = 1.0f / 30;
        std::memcpy(img.data() + bound_at, &bound, 4);
        m = ReadMainStep(img.data(), img.size());
        CHECK(m.form == MainStep::Clamp && Near(m.bound, 1.0 / 30));
        CHECK(Near(StepFor(m, 1.0f / 120, 0.0f), 1.0 / 120)); // target 0: the measurement
        CHECK(Near(StepFor(m, 0.5f, 0.0f), 1.0 / 30));
        CHECK(Near(StepFor(m, 1.0f / 120, 1.0f / 60), 1.0 / 60));
        CHECK(Near(StepFor(m, NAN, 0.0f), ShortestFrame));    // never below the shortest frame
        CHECK(ReadMainStep(img.data(), bound_at + 2).form == MainStep::Unknown); // bound outside
        // Any other bytes: unknown.
        code[40] = 0xcc;
        CHECK(ReadMainStep(img.data(), img.size()).form == MainStep::Unknown);
        std::memcpy(at + 7, unless30, sizeof(unless30));
        at[7 + 30] ^= 1;
        CHECK(ReadMainStep(img.data(), img.size()).form == MainStep::Unknown);
        std::memcpy(at + 7, game, sizeof(game));
        CHECK(ReadMainStep(img.data(), img.size()).form == MainStep::Fixed);
        at[0] = 0x90;
        CHECK(ReadMainStep(img.data(), img.size()).form == MainStep::Unknown);
        CHECK(ReadMainStep(img.data(), MainStepEnd - 1).form == MainStep::Unknown);
    }

    // Operands: a 2-byte and a 3-byte VEX instruction reading a constant RIP-relative.
    std::vector<unsigned char> image(0x10000, 0x90);
    const auto put_disp = [&](std::size_t at, std::size_t length, std::size_t constant) {
        const std::int32_t d = std::int32_t(constant) - std::int32_t(at + length);
        std::memcpy(&image[at + length - 4], &d, 4);
    };
    const unsigned char vmulss[] = {0xc5, 0xfa, 0x59, 0x05}; // vmulss xmm0, xmm0, [rip + d]
    const unsigned char vmovss3[] = {0xc4, 0xe1, 0x7a, 0x10, 0x05}; // vmovss xmm0, [rip + d]
    std::memcpy(&image[0x100], vmulss, 4);
    put_disp(0x100, 8, 0x8000);
    std::memcpy(&image[0x200], vmovss3, 5);
    put_disp(0x200, 9, 0x8004);
    std::memcpy(&image[0x300], vmulss, 4);
    put_disp(0x300, 8, 0x8008);
    const std::vector<unsigned char> before = image;
    CHECK(RedirectOperand(image.data(), 0x100, 0x8000, image.data() + 0x9000));
    std::int32_t d = 0;
    std::memcpy(&d, &image[0x104], 4);
    CHECK(d == 0x9000 - 0x108);
    CHECK(RedirectOperand(image.data(), 0x200, 0x8004, image.data() + 0x9010));
    std::memcpy(&d, &image[0x205], 4);
    CHECK(d == 0x9010 - 0x209);
    CHECK(!RedirectOperand(image.data(), 0x300, 0x8000, image.data() + 0x9000)); // other constant
    CHECK(!RedirectOperand(image.data(), 0x400, 0x8000, image.data() + 0x9000)); // not VEX
    CHECK(std::memcmp(&image[0x300], &before[0x300], 0x200) == 0);
    CHECK(std::memcmp(&image[0], &before[0], 0x104) == 0 && image[0x108] == before[0x108]);

    // The fixed step's detour, run: a function shaped like the site's.
    unsigned char* x = Executable(0x2000);
    CHECK(x != nullptr);
    if (x) {
        const std::uint32_t old_step = Bits(1.0f / 240);
        unsigned char* scratch = x + 0x1000;
        unsigned char fn[] = {
            0x55,                                     // push rbp
            0x48, 0x89, 0xe5,                         // mov rbp, rsp
            0x41, 0x54,                               // push r12
            0x57,                                     // push rdi (kept on Windows)
            0x48, 0x83, 0xec, 0x30,                   // sub rsp, 0x30
            0x49, 0xbc, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs r12, scratch
            0xc7, 0x45, 0xd0, 0, 0, 0, 0,             // mov dword [rbp - 0x30], step (the site)
            0x49, 0x8b, 0xbc, 0x24, 0xd0, 0, 0, 0,    // mov rdi, [r12 + 0xd0]
            0x8b, 0x45, 0xd0,                         // mov eax, [rbp - 0x30]
            0x01, 0xf8,                               // add eax, edi (what the next one loaded)
            0x48, 0x83, 0xc4, 0x30,                   // add rsp, 0x30
            0x5f,                                     // pop rdi
            0x41, 0x5c,                               // pop r12
            0x5d,                                     // pop rbp
            0xc3,                                     // ret
        };
        const std::uintptr_t s = reinterpret_cast<std::uintptr_t>(scratch);
        std::memcpy(fn + 13, &s, 8);
        std::memcpy(fn + 24, &old_step, 4);
        constexpr std::size_t site = 21;
        std::memcpy(x, fn, sizeof(fn));
        const std::uint64_t loaded = 0x100; // [r12 + 0xd0]: the instruction after the site ran
        std::memcpy(scratch + 0xd0, &loaded, 8);
        using Fn = std::uint32_t (*)();
        const Fn run = reinterpret_cast<Fn>(x);
        CHECK(run() == old_step + 0x100);
        float* value = reinterpret_cast<float*>(x + 0x900);
        *value = 1.0f / 120;
        CHECK(DetourStepStore(x, site, x + 0x800, value));
        CHECK(run() == Bits(1.0f / 120) + 0x100);
        *value = 1.0f / 90; // read at each run, not copied
        CHECK(run() == Bits(1.0f / 90) + 0x100);
        CHECK(x[site] == 0xe9 && x[site + 5] == 0x66 && x[site + 6] == 0x90);

        // Other bytes after the store (edi not known free): refused, nothing written.
        unsigned char other[sizeof(fn)];
        std::memcpy(other, fn, sizeof(fn));
        other[site + 7] = 0x48; // mov rdi, ... with another REX: not the expected instruction
        std::memcpy(x + 0x400, other, sizeof(other));
        CHECK(!DetourStepStore(x + 0x400, site, x + 0x880, value));
        CHECK(std::memcmp(x + 0x400, other, sizeof(other)) == 0);
    }

    std::printf("frame rate values: %s\n", failed ? "FAILED" : "PASS");
    return failed != 0;
}
