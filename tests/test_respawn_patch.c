#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "respawn_patch.h"

static const size_t image_size = 0x49280cc, init_at = 0x1938b59, patch_at = 0x1938b64;
static const unsigned char init[] = {
    0x41,0x80,0xbc,0x24,0xe4,0,0,0,0,0x74,0x0a,
    0xc5,0xfa,0x10,0x05,0x5c,0xf5,0xfe,0x02,0xeb,0x04,0xc5,0xf8,0x57,0xc0,
    0xc4,0xc1,0x7a,0x11,0x84,0x24,0x78,0x02,0,0,
    0x41,0xc7,0x84,0x24,0x7c,0x02,0,0,0,0,0x70,0x41
};
static const unsigned char ready[] = {
    0xc5,0xf8,0x2e,0xca,0x0f,0x87,0x53,0x06,0,0,
    0x45,0x84,0xff,0x0f,0x84,0x4a,0x06,0,0
};

/* Run the original guest instructions with a Windows ABI adapter. Only the
 * original RIP-relative constant is relocated into this executable allocation.
 */
static void check_initialization(const unsigned char *image, float death_delay) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(code);
    const unsigned char prologue[] = {0x41,0x54,0x49,0x89,0xcc}; /* push r12; mov r12,rcx */
    memcpy(code, prologue, sizeof(prologue));
    memcpy(code + sizeof(prologue), image + init_at, sizeof(init));
    if (image[patch_at + 1] == 0xfa) {
        int32_t displacement = (int32_t)(128 - (sizeof(prologue) + 19));
        memcpy(code + sizeof(prologue) + 15, &displacement, 4);
    }
    const unsigned char epilogue[] = {0x41,0x5c,0xc3}; /* pop r12; ret */
    memcpy(code + sizeof(prologue) + sizeof(init), epilogue, sizeof(epilogue));
    memcpy(code + 128, image + 0x49280c8, 4);
    DWORD old;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), code, 4096));
    void (*run)(void *) = (void (*)(void *))(uintptr_t)code;
    for (unsigned dead = 0; dead < 2; ++dead) {
        unsigned char state[0x300], expected[sizeof(state)];
        memset(state, 0xa5, sizeof(state));
        state[0xe4] = (unsigned char)dead;
        memcpy(expected, state, sizeof(state));
        float minimum = dead ? death_delay : 0.0f, timeout = 15.0f;
        memcpy(expected + 0x278, &minimum, 4);
        memcpy(expected + 0x27c, &timeout, 4);
        run(state);
        assert(!memcmp(state, expected, sizeof(state)));
    }
    assert(VirtualFree(code, 0, MEM_RELEASE));
}

/* Retain the real countdown/character-ready branches, including their original
 * jump distances. The two return stubs replace the rest of the loading step.
 */
static void check_readiness(const unsigned char *image) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(code);
    const unsigned char prologue[] = {
        0x41,0x57,0x41,0x89,0xd7, /* push r15; mov r15d,edx */
        0xc5,0xfa,0x10,0x89,0x78,0x02,0,0, /* vmovss xmm1,[rcx+278] */
        0xc5,0xe8,0x57,0xd2 /* vxorps xmm2,xmm2,xmm2 */
    };
    const unsigned char allow[] = {0xb8,1,0,0,0,0x41,0x5f,0xc3};
    const unsigned char wait[] = {0x31,0xc0,0x41,0x5f,0xc3};
    memcpy(code, prologue, sizeof(prologue));
    memcpy(code + sizeof(prologue), image + 0x193a581, sizeof(ready));
    memcpy(code + sizeof(prologue) + sizeof(ready), allow, sizeof(allow));
    memcpy(code + sizeof(prologue) + 0x65d, wait, sizeof(wait));
    DWORD old;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    assert(FlushInstructionCache(GetCurrentProcess(), code, 4096));
    int (*run)(void *, int) = (int (*)(void *, int))(uintptr_t)code;
    unsigned char state[0x300] = {0};
    assert(run(state, 0) == 0); /* no minimum delay still waits for characters */
    assert(run(state, 1) == 1);
    float positive = 1.0f;
    memcpy(state + 0x278, &positive, 4);
    assert(run(state, 0) == 0);
    assert(run(state, 1) == 0);
    assert(VirtualFree(code, 0, MEM_RELEASE));
}

int main(int argc, char **argv) {
    unsigned char *image = calloc(1, image_size);
    assert(image);
    if (argc == 3 && !strcmp(argv[1], "--image")) {
        FILE *f = fopen(argv[2], "rb");
        assert(f && fread(image, 1, image_size, f) == image_size);
        fclose(f);
    } else {
        assert(argc == 1);
        memcpy(image + init_at, init, sizeof(init));
        memcpy(image + 0x193a581, ready, sizeof(ready));
        const float twelve = 12.0f;
        memcpy(image + 0x49280c8, &twelve, 4);
    }
    assert(!memcmp(image + init_at, init, sizeof(init)));
    assert(!memcmp(image + 0x193a581, ready, sizeof(ready)));
    check_initialization(image, 12.0f);
    check_readiness(image);
    assert(bb_patch_respawn_delay(NULL, image_size) == BB_RESPAWN_UNSUPPORTED);
    for (size_t short_size = 0; short_size < image_size; short_size += 0x100001)
        assert(bb_patch_respawn_delay(image, short_size) == BB_RESPAWN_UNSUPPORTED);
    assert(bb_patch_respawn_delay(image, image_size - 1) == BB_RESPAWN_UNSUPPORTED);
    const struct { size_t at, size; } guards[] = {
        {init_at, sizeof(init)}, {0x193a581, sizeof(ready)}, {0x49280c8, 4}
    };
    for (size_t g = 0; g < sizeof(guards) / sizeof(*guards); ++g) {
        for (size_t i = 0; i < guards[g].size; ++i) {
            unsigned char *at = image + guards[g].at + i;
            *at ^= 1;
            assert(bb_patch_respawn_delay(image, image_size) == BB_RESPAWN_UNSUPPORTED);
            *at ^= 1;
            assert(!memcmp(image + init_at, init, sizeof(init)));
        }
    }
    unsigned char *before = malloc(image_size);
    assert(before);
    memcpy(before, image, image_size);
    assert(bb_patch_respawn_delay(image, image_size) == BB_RESPAWN_APPLIED);
    assert(!memcmp(before, image, patch_at));
    assert(!memcmp(before + patch_at + 8, image + patch_at + 8, image_size - patch_at - 8));
    check_initialization(image, 0.0f);
    check_readiness(image);
    memcpy(before, image, image_size);
    assert(bb_patch_respawn_delay(image, image_size) == BB_RESPAWN_ALREADY_APPLIED);
    assert(!memcmp(before, image, image_size));
    free(before);
    free(image);
    puts("PASS: actual loading instructions, 12s removal, readiness/15s timeout, guards and idempotence");
    return 0;
}
