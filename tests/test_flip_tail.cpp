// Tests BbKeepAfterGarbage (gpu/shim/bbport_flip_tail.h): which part of a
// command buffer the draw engine keeps after a garbage packet header.
#include "gpu/shim/bbport_flip_tail.h"

#include <cstdio>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

static std::uint32_t type3(std::uint32_t opcode, std::uint32_t count) {
    return (3u << 30) | ((count & 0x3fff) << 16) | (opcode << 8);
}

// A buffer of SIZE dwords whose last 64 hold what PatchFlipRequest writes: WriteData (lock), Nop
// with the PatchedFlip payload.
static std::vector<std::uint32_t> flip_buffer(std::size_t size) {
    std::vector<std::uint32_t> buffer(size, 0xdeadbeefu);
    const std::size_t tail = size - 64;
    buffer[tail] = type3(0x37, 3);
    buffer[tail + 5] = type3(0x10, 0x39);
    buffer[tail + 6] = 0x68750776u;
    return buffer;
}

int main() {
    {
        const auto buffer = flip_buffer(1000);
        const std::span<const std::uint32_t> whole{buffer};
        const auto kept = BbKeepAfterGarbage(whole, buffer.data() + 10);
        check(kept.data() == buffer.data() + 936 && kept.size() == 64, "garbage before the flip keeps the last 64");
        check(BbKeepAfterGarbage(whole, buffer.data()).size() == 64, "garbage at the start keeps the flip");
        check(BbKeepAfterGarbage(whole, buffer.data() + 935).size() == 64, "garbage just before the flip keeps it");
        check(BbKeepAfterGarbage(whole, buffer.data() + 936).empty(), "garbage at the flip drops it");
        check(BbKeepAfterGarbage(whole, buffer.data() + 990).empty(), "garbage inside the flip drops the rest");
    }
    {
        auto buffer = flip_buffer(1000);
        buffer[936 + 6] = 0x68750777u; // PrepareFlip: not patched
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{buffer}, buffer.data() + 10).empty(),
              "an unpatched flip marker is not kept");
    }
    {
        auto buffer = flip_buffer(1000);
        buffer[936] = type3(0x10, 3); // the lock is not a WriteData
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{buffer}, buffer.data() + 10).empty(),
              "no lock packet, nothing kept");
    }
    {
        auto buffer = flip_buffer(1000);
        buffer[936 + 5] = type3(0x37, 0x39); // a WriteData where the Nop belongs
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{buffer}, buffer.data() + 10).empty(),
              "no flip Nop, nothing kept");
    }
    {
        std::vector<std::uint32_t> buffer(1000, 0xdeadbeefu); // an ordinary buffer, no flip
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{buffer}, buffer.data() + 10).empty(),
              "a buffer without a flip drops the rest");
    }
    {
        const auto buffer = flip_buffer(64); // only the flip
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{buffer}, buffer.data()).empty(),
              "garbage at the flip of a flip-only buffer drops it");
        std::vector<std::uint32_t> short_buffer(10, 0);
        check(BbKeepAfterGarbage(std::span<const std::uint32_t>{short_buffer}, short_buffer.data()).empty(),
              "a buffer shorter than 64 dwords keeps nothing");
    }
    {
        const auto big = flip_buffer(2000); // the buffer is its second half, the flip at its end
        const auto whole = std::span<const std::uint32_t>{big}.subspan(1000);
        check(BbKeepAfterGarbage(whole, big.data() + 10).empty(), "a position before the buffer keeps nothing");
        check(BbKeepAfterGarbage(whole, big.data() + 1010).size() == 64, "the same buffer keeps its flip");
    }
    std::printf("%s: %d failed\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
