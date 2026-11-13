// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the refill cheat (bbport_refill.h). The game's data and functions it uses were found
// for BBSC, the seamless co-op mod (its bloodborne_re.cpp: the boss rewards and the restock after
// an in-place respawn give items the same way); offsets are into the game's image, 1.09.

#include "bbport_refill.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "bbport_settings.h"
#include "common/types.h"

#ifdef _WIN32
#include <windows.h>
#endif

// The hook (Windows): the game's per-frame step (0x2018d20, the main loop's; it runs the frame
// limiter and then the frame's tasks, so the game's own updates are not running) jumps here
// from its entry. The stub calls bbrefill_tick on the game's thread and stack, then runs the
// prologue it replaced and jumps back. The game's registers are kept: everything the Windows ABI
// lets bbrefill_tick change and the SysV argument registers (rdi, rsi, xmm0-7) are saved.
#ifdef _WIN32
extern "C" {
uintptr_t bbrefill_return;
void bbrefill_tick();
extern char bbrefill_stub[];
}
__asm__(".text\n"
        ".globl bbrefill_stub\nbbrefill_stub:\n"
        "  lea -128(%rsp), %rsp\n  pushfq\n"
        "  push %rax\n  push %rcx\n  push %rdx\n  push %rsi\n  push %rdi\n  push %r8\n"
        "  push %r9\n  push %r10\n  push %r11\n  push %rbx\n"
        "  mov %rsp, %rbx\n  and $-16, %rsp\n  sub $0xa0, %rsp\n"
        "  movdqu %xmm0, 0x20(%rsp)\n  movdqu %xmm1, 0x30(%rsp)\n  movdqu %xmm2, 0x40(%rsp)\n"
        "  movdqu %xmm3, 0x50(%rsp)\n  movdqu %xmm4, 0x60(%rsp)\n  movdqu %xmm5, 0x70(%rsp)\n"
        "  movdqu %xmm6, 0x80(%rsp)\n  movdqu %xmm7, 0x90(%rsp)\n"
        "  call bbrefill_tick\n"
        "  movdqu 0x20(%rsp), %xmm0\n  movdqu 0x30(%rsp), %xmm1\n  movdqu 0x40(%rsp), %xmm2\n"
        "  movdqu 0x50(%rsp), %xmm3\n  movdqu 0x60(%rsp), %xmm4\n  movdqu 0x70(%rsp), %xmm5\n"
        "  movdqu 0x80(%rsp), %xmm6\n  movdqu 0x90(%rsp), %xmm7\n"
        "  mov %rbx, %rsp\n"
        "  pop %rbx\n  pop %r11\n  pop %r10\n  pop %r9\n  pop %r8\n  pop %rdi\n  pop %rsi\n"
        "  pop %rdx\n  pop %rcx\n  pop %rax\n  popfq\n  lea 128(%rsp), %rsp\n"
        // the replaced prologue
        "  push %rbp\n  mov %rsp, %rbp\n  push %r15\n  push %r14\n  push %r13\n  push %r12\n"
        "  push %rbx\n  sub $0x18, %rsp\n"
        "  jmp *bbrefill_return(%rip)\n");
#endif

namespace BbRefill {
namespace {

constexpr u64 FrameStepOffset = 0x2018d20;
// push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x18
constexpr u8 FrameStepPrologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x18};
constexpr size_t JumpSize = 14; // jmp [rip+0]; dq target

// [[0x0553B130]+8]: the local player's game data; +0x328 the carried items.
constexpr u64 PlayerSlotsPointerOffset = 0x0553B130;
// [[0x0553E878]+0x60]: the local player's character; HP [[[chr+0x3B0]+0x20]+0xF8].
constexpr u64 WorldCharactersPointerOffset = 0x0553E878;
// (inventory, &code, count) -> how many of `count` fit; (inventory, &key, count) adds them.
constexpr u64 InventoryFitsOffset = 0x014D9A80;
constexpr u64 InventoryAddOffset = 0x014DA0A0;

constexpr u32 BloodVialGoods = 1000;
constexpr u32 QuicksilverBulletGoods = 900;
constexpr s32 Wanted = 20;
// Alive this long after a spawn before the top-up: the load has finished and the game has
// restocked from the storage box itself (lamp rest, death).
constexpr s64 SettleMs = 2000;

uintptr_t image_base = 0;

#ifdef _WIN32
bool Readable(u64 address, size_t size) {
    if (address < 0x10000 || address + size < address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    return address + size <= reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
}
#else
bool Readable(u64 address, size_t size) {
    return address >= 0x10000 && address + size >= address;
}
#endif

template <typename T>
T Read(u64 address) {
    return Readable(address, sizeof(T)) ? *reinterpret_cast<const volatile T*>(address) : T{};
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u64 LocalPlayer() {
    const u64 characters = Read<u64>(image_base + WorldCharactersPointerOffset);
    return characters != 0 ? Read<u64>(characters + 0x60) : 0;
}

/// -1 when the character's stats are not there (yet).
s32 Hp(u64 character) {
    const u64 modules = Read<u64>(character + 0x3B0);
    const u64 stats = modules != 0 ? Read<u64>(modules + 0x20) : 0;
    return stats != 0 && Readable(stats + 0xF8, 4) ? Read<s32>(stats + 0xF8) : -1;
}

u64 CarriedItems() {
    const u64 slots = Read<u64>(image_base + PlayerSlotsPointerOffset);
    const u64 data = slots != 0 ? Read<u64>(slots + 0x08) : 0;
    return data != 0 && Readable(data + 0x328, 0x90) ? data + 0x328 : 0;
}

/// The inventory's own lookup (as 0x014D9A80 does it): the hash [inv+0x68][code % 0x7E1] (s16,
/// < 0: none) indexes the entries at [inv+0x78] (u32 code, u32 slot in bits 0-11 and next + 1 in
/// bits 12-23); slots below [inv+0x24] are at [inv+0x58] + slot * 16, the others at [inv+0x48].
/// The count of `code` (0x40000000 | goods id), 0 when none.
s32 Count(u64 inventory, u32 code) {
    const u64 hashes = Read<u64>(inventory + 0x68);
    const u64 entries = Read<u64>(inventory + 0x78);
    if (!Readable(hashes, 0x7E1 * 2) || entries == 0) {
        return 0;
    }
    s64 index = Read<s16>(hashes + (code % 0x7E1) * 2);
    for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
        const u64 entry = entries + static_cast<u64>(index) * 8;
        if (!Readable(entry, 8)) {
            return 0;
        }
        const u32 info = Read<u32>(entry + 4);
        if (Read<u32>(entry) == code) {
            const u32 slot = info & 0xFFF;
            const u32 split = Read<u32>(inventory + 0x24);
            const u64 at = slot < split ? Read<u64>(inventory + 0x58) + slot * 16ull
                                        : Read<u64>(inventory + 0x48) + (slot - split) * 16ull;
            return Readable(at, 16) && Read<u32>(at) != 0 ? Read<s32>(at + 8) : 0;
        }
        const u32 next = (((info >> 12) & 0xFFF) + 0xFFFF) & 0xFFFF; // next + 1 stored; 0 = end
        if ((next & 0x8000) != 0) {
            return 0;
        }
        index = next;
    }
    return 0;
}

void TopUp() {
    const u64 carried = CarriedItems();
    if (carried == 0) {
        std::printf("Refill cheat: the carried items were not found\n");
        return;
    }
    using Fits = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* code, s32 count);
    using Add = s32 PS4_SYSV_ABI (*)(u64 inventory, const u32* key, s32 count);
    alignas(16) static u32 code = 0;
    alignas(16) static u32 key = 0;
    for (const u32 goods : {BloodVialGoods, QuicksilverBulletGoods}) {
        const char* name = goods == BloodVialGoods ? "Blood Vials" : "Quicksilver Bullets";
        code = 0x40000000u | goods;
        const s32 before = Count(carried, code);
        if (before >= Wanted) {
            continue;
        }
        const s32 fits = reinterpret_cast<Fits>(image_base + InventoryFitsOffset)(carried, &code,
                                                                                   Wanted - before);
        const s32 give = std::min(fits, Wanted - before);
        if (give <= 0) {
            std::printf("Refill cheat: %s %d (no room for more)\n", name, before);
            continue;
        }
        key = 0x80000000u | (3u << 28) | goods;
        reinterpret_cast<Add>(image_base + InventoryAddOffset)(carried, &key, give);
        std::printf("Refill cheat: %s %d -> %d\n", name, before, Count(carried, code));
    }
}

// The game's thread, every frame. A spawn is the local player appearing (a load), becoming
// another character, or coming back alive; it counts once the player has stayed alive for
// SettleMs. With the cheat off the player is still followed, so turning it on in the middle of
// a level waits for the next spawn.
[[maybe_unused]] void Tick() {
    static u64 last_player = 0;
    static bool absent_or_dead = true;
    static s64 spawned_ms = -1;
    const u64 player = LocalPlayer();
    if (player == 0 || Hp(player) <= 0) {
        absent_or_dead = true;
        spawned_ms = -1;
        return;
    }
    if (player != last_player || absent_or_dead) {
        last_player = player;
        absent_or_dead = false;
        spawned_ms = NowMs();
    }
    if (spawned_ms < 0 || NowMs() - spawned_ms < SettleMs) {
        return;
    }
    spawned_ms = -1;
    if (BbSettings::Get().cheat_refill.load(std::memory_order_relaxed)) {
        TopUp();
    }
}

} // namespace

void Install(unsigned char* image, std::uint64_t size) {
#ifdef _WIN32
    const uintptr_t site = reinterpret_cast<uintptr_t>(image) + FrameStepOffset;
    if (image == nullptr || size < FrameStepOffset + sizeof(FrameStepPrologue) ||
        std::memcmp(reinterpret_cast<const void*>(site), FrameStepPrologue,
                    sizeof(FrameStepPrologue)) != 0) {
        std::printf("Refill cheat: the game's frame step was not found (game 1.09 needed)\n");
        return;
    }
    image_base = reinterpret_cast<uintptr_t>(image);
    bbrefill_return = site + sizeof(FrameStepPrologue);
    u8 jump[sizeof(FrameStepPrologue)];
    std::memset(jump, 0x90, sizeof(jump));
    jump[0] = 0xFF; // jmp [rip+0]
    jump[1] = 0x25;
    std::memset(jump + 2, 0, 4);
    const uintptr_t stub = reinterpret_cast<uintptr_t>(bbrefill_stub);
    std::memcpy(jump + 6, &stub, 8);
    static_assert(sizeof(FrameStepPrologue) >= JumpSize);
    DWORD old_protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(jump), PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        std::printf("Refill cheat: the game's code is not writable\n");
        return;
    }
    std::memcpy(reinterpret_cast<void*>(site), jump, sizeof(jump));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(site), sizeof(jump), old_protect, &unused);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(jump));
    std::printf("Refill cheat: frame step hooked (%s)\n",
                BbSettings::Get().cheat_refill.load() ? "on" : "off");
#else
    (void)image;
    (void)size;
#endif
}

} // namespace BbRefill

#ifdef _WIN32
extern "C" void bbrefill_tick() {
    BbRefill::Tick();
}
#endif
