#ifndef BB_RESPAWN_PATCH_H
#define BB_RESPAWN_PATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Bloodborne 1.09: the player-death flag sets a 12-second minimum loading time that the
 * world-loading step waits for even once the characters are ready. Only its initialization
 * changes (vmovss xmm0,[12.0] becomes vxorps xmm0,xmm0,xmm0); the readiness branch, the loading
 * steps and the 15-second multiplayer timeout stay. The code is found by pattern: its address
 * differs between releases and with the modules linked into the image. */

enum { BB_RESPAWN_ORIGINAL = 1, BB_RESPAWN_ALREADY_APPLIED = 2 };

enum {
    BB_RESPAWN_LOAD_AT = 11,     /* vmovss xmm0,[rip+disp32], 8 bytes */
    BB_RESPAWN_TAIL_AT = 19,
    BB_RESPAWN_SIZE = 47,
    BB_RESPAWN_READY_AT = 0x1a28 /* the countdown test, then the character readiness test */
};

static const unsigned char bb_respawn_head[BB_RESPAWN_LOAD_AT] = {
    0x41,0x80,0xbc,0x24,0xe4,0x00,0x00,0x00,0x00, /* cmp byte [r12+0xe4],0 (death flag) */
    0x74,0x0a,                                    /* je  -> xorps */
};
static const unsigned char bb_respawn_load[4] = {0xc5,0xfa,0x10,0x05};
static const unsigned char bb_respawn_zero[8] = {0xc5,0xf8,0x57,0xc0,0x0f,0x1f,0x40,0x00};
static const unsigned char bb_respawn_tail[BB_RESPAWN_SIZE - BB_RESPAWN_TAIL_AT] = {
    0xeb,0x04,0xc5,0xf8,0x57,0xc0,                          /* jmp; vxorps xmm0 */
    0xc4,0xc1,0x7a,0x11,0x84,0x24,0x78,0x02,0x00,0x00,      /* vmovss [r12+0x278],xmm0 */
    0x41,0xc7,0x84,0x24,0x7c,0x02,0x00,0x00,0x00,0x00,0x70,0x41 /* [r12+0x27c] = 15.0 */
};
static const unsigned char bb_respawn_ready[19] = {
    0xc5,0xf8,0x2e,0xca,0x0f,0x87,0x53,0x06,0x00,0x00,
    0x45,0x84,0xff,0x0f,0x84,0x4a,0x06,0x00,0x00
};
static const unsigned char bb_respawn_twelve[4] = {0x00,0x00,0x40,0x41};

/* BB_RESPAWN_ORIGINAL, BB_RESPAWN_ALREADY_APPLIED or 0 for the code at `at`. */
static inline int bb_respawn_match(const unsigned char *image, size_t size, size_t at) {
    if (at > size || size - at < BB_RESPAWN_READY_AT + sizeof(bb_respawn_ready)) return 0;
    const unsigned char *p = image + at;
    if (memcmp(p, bb_respawn_head, sizeof(bb_respawn_head)) ||
        memcmp(p + BB_RESPAWN_TAIL_AT, bb_respawn_tail, sizeof(bb_respawn_tail)) ||
        memcmp(p + BB_RESPAWN_READY_AT, bb_respawn_ready, sizeof(bb_respawn_ready)))
        return 0;
    if (!memcmp(p + BB_RESPAWN_LOAD_AT, bb_respawn_zero, sizeof(bb_respawn_zero)))
        return BB_RESPAWN_ALREADY_APPLIED;
    if (memcmp(p + BB_RESPAWN_LOAD_AT, bb_respawn_load, sizeof(bb_respawn_load))) return 0;
    int32_t disp;
    memcpy(&disp, p + BB_RESPAWN_LOAD_AT + 4, 4);
    const int64_t constant = (int64_t)(at + BB_RESPAWN_TAIL_AT) + disp;
    if (constant < 0 || (uint64_t)constant > size - sizeof(bb_respawn_twelve)) return 0;
    return memcmp(image + constant, bb_respawn_twelve, sizeof(bb_respawn_twelve)) ? 0
                                                                                  : BB_RESPAWN_ORIGINAL;
}

/* Number of matches in [begin, end); `*at` and `*state` describe the last one. */
static inline unsigned bb_respawn_find(const unsigned char *image, size_t size, size_t begin,
                                       size_t end, size_t *at, int *state) {
    unsigned found = 0;
    if (end > size) end = size;
    while (begin + BB_RESPAWN_SIZE <= end) {
        const unsigned char *hit = memchr(image + begin, bb_respawn_head[0], end - begin - BB_RESPAWN_SIZE + 1);
        if (!hit) break;
        const size_t offset = (size_t)(hit - image);
        const int match = bb_respawn_match(image, size, offset);
        if (match) {
            ++found;
            *at = offset;
            *state = match;
        }
        begin = offset + 1;
    }
    return found;
}

static inline void bb_respawn_apply(unsigned char *image, size_t at) {
    memcpy(image + at + BB_RESPAWN_LOAD_AT, bb_respawn_zero, sizeof(bb_respawn_zero));
}

#endif
