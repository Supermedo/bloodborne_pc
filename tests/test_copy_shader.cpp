// Test of gpu/shim/bbport_copy_shader.h:
// the buffers of a crash do not fit the copy shader while the ones it always had do
// (both taken from the permutations in that run's pipeline cache), and neither do empty buffers,
// other strides or other formats. A control record inside all limits copies what the shortcut copied
// before; one past a limit (64 elements per record, the buffers' record counts) is cut to what is
// inside or to nothing, and a record of end 0xffffffff does not wrap around to a copy of no bytes.
// Reports stop being written after a few dozen.
//
#include "gpu/shim/bbport_copy_shader.h"

#include <array>
#include <cstdint>
#include <cstdio>

static int failed = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                       \
            ++failed;                                                                              \
        }                                                                                          \
    } while (0)

using namespace BbCopyShader;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Sharp = std::array<u32, 4>;

// amdgpu/resource.h Buffer: the stride is 14 bits at bit 48 of the first two words, the number and
// data formats 3 and 4 bits at bits 12 and 15 of the last word.
static Elements Decode(const Sharp& sharp) {
    return {(sharp[1] >> 16) & 0x3fff, (sharp[3] >> 15) & 15, (sharp[3] >> 12) & 7};
}

int main() {
    // The user data of permutation 0 of shader 0xfefebf9f, the only one before that run.
    constexpr Sharp NormalControl{0x4d5bd040, 0x000cb010, 0x00000400, 0x0006c1ac};
    constexpr Sharp NormalSource{0x4d5c0040, 0x0004b010, 0x00010000, 0x00024004};
    constexpr Sharp NormalDestination{0x39e3bd00, 0x0004b011, 0x00010000, 0x00024004};
    CHECK(ShapeFits(Decode(NormalControl), Decode(NormalSource), Decode(NormalDestination)));

    // Permutation 1, written in the second the game stopped.
    constexpr Sharp CrashControl{0x437865d0, 0x00108010, 0x00000001, 0x00077fac};
    constexpr Sharp CrashSource{0xb09c0000, 0x00043010, 0x00400000, 0x0002f22c};
    constexpr Sharp CrashDestination{0x522ed940, 0x00048010, 0x39f3fddc, 0x00024004};
    CHECK(!ShapeFits(Decode(CrashControl), Decode(CrashSource), Decode(CrashDestination)));

    constexpr Elements Control12{12, ControlDataFormat, ControlNumberFormat};
    constexpr Elements R32{4, 4, 4};
    CHECK(ShapeFits(Control12, R32, R32));
    CHECK(!ShapeFits({}, {}, {}));                       // registers holding nothing
    CHECK(!ShapeFits(Control12, {}, {}));                // a copy of no bytes per element
    CHECK(!ShapeFits(Control12, R32, {8, 11, 4}));       // the ASSERTs' other condition
    CHECK(!ShapeFits({16, 14, 7}, R32, R32));            // the crash's control buffer
    CHECK(!ShapeFits({12, 13, 7}, R32, R32));            // the right stride, floats for indices
    CHECK(!ShapeFits({12, 11, 4}, R32, R32));            // the right stride, another layout
    CHECK(!ShapeFits(Control12, {4, 5, 7}, R32));        // the same stride, not the same elements
    CHECK(ShapeFits(Control12, {16, 14, 4}, {16, 14, 4})); // any one element type, as before

    // Inside every limit: exactly the old offsets and size, nothing cut.
    {
        const Copy copy = Range({.dst_idx = 100, .src_idx = 200, .end = 9}, 4, 1000, 1000);
        CHECK(copy.src_offset == 800);
        CHECK(copy.dst_offset == 400);
        CHECK(copy.size == 40);
        CHECK(!copy.cut);
    }
    // Up to the last element of both buffers.
    {
        const Copy copy = Range({.dst_idx = 990, .src_idx = 990, .end = 9}, 4, 1000, 1000);
        CHECK(copy.size == 40);
        CHECK(!copy.cut);
    }
    // The shader's threads: 64 elements are a whole record, 65 are not.
    {
        const Copy whole = Range({.dst_idx = 0, .src_idx = 0, .end = 63}, 4, 1000, 1000);
        CHECK(whole.size == 256);
        CHECK(!whole.cut);
        const Copy more = Range({.dst_idx = 0, .src_idx = 0, .end = 64}, 4, 1000, 1000);
        CHECK(more.size == 256);
        CHECK(more.cut);
    }
    // One element past the destination's end, then past the source's.
    {
        const Copy copy = Range({.dst_idx = 991, .src_idx = 0, .end = 9}, 4, 1000, 1000);
        CHECK(copy.size == 36);
        CHECK(copy.cut);
        const Copy other = Range({.dst_idx = 0, .src_idx = 995, .end = 9}, 4, 1000, 1000);
        CHECK(other.size == 20);
        CHECK(other.cut);
    }
    // Starting past an end: nothing to copy.
    CHECK(Range({.dst_idx = 1000, .src_idx = 0, .end = 0}, 4, 1000, 1000).size == 0);
    CHECK(Range({.dst_idx = 0, .src_idx = 1000, .end = 0}, 4, 1000, 1000).size == 0);
    CHECK(Range({.dst_idx = 0xffffffff, .src_idx = 0xffffffff, .end = 0}, 4, 1000, 1000).size == 0);
    CHECK(Range({.dst_idx = 0, .src_idx = 0, .end = 0}, 4, 0, 1000).size == 0);
    // end + 1 and the offsets used to wrap around in 32 bits.
    {
        const Copy copy = Range({.dst_idx = 0, .src_idx = 0, .end = 0xffffffff}, 4, 65536, 65536);
        CHECK(copy.size == 256);
        CHECK(copy.cut);
        const Copy wide = Range({.dst_idx = 0x7fffffff, .src_idx = 0x7ffffff0, .end = 0}, 4,
                                0xffffffff, 0xffffffff);
        CHECK(wide.src_offset == u64{0x7ffffff0} * 4);
        CHECK(wide.dst_offset == u64{0x7fffffff} * 4);
        CHECK(wide.size == 4);
        CHECK(!wide.cut);
    }

    // The log: the first eight, then rarely.
    u32 reports = 0;
    for (u64 count = 1; count <= 10'000'000; ++count) {
        reports += ShouldReport(count) ? 1 : 0;
    }
    CHECK(ShouldReport(1));
    CHECK(ShouldReport(8));
    CHECK(!ShouldReport(9));
    CHECK(ShouldReport(16));
    CHECK(!ShouldReport(17));
    CHECK(reports <= 8 + 24);

    if (failed) {
        std::printf("%d failed\n", failed);
        return 1;
    }
    std::printf("copy shader: all passed\n");
    return 0;
}
