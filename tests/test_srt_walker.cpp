// SPDX-License-Identifier: GPL-2.0-or-later
// The SRT walker the recompiler generates, run against tables that change from draw to draw:
// a table pointer that is null (or bad) in one draw reads zero in that draw only. Upstream
// rewrote the faulting load for good, and the next draws read zero too (Ebrietas: the map gone
// until a restart). No GPU, no game.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// As the loader does (probe.c): access violations go to the registered handlers first.
#ifdef _WIN32
LONG CALLBACK Fault(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
        Core::Signals::Instance()->DispatchAccessViolation(
            info, reinterpret_cast<void*>(record->ExceptionInformation[1]))) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void InstallFaultHandler() {
    AddVectoredExceptionHandler(1, Fault);
}
#else
void Fault(int, siginfo_t* info, void* context) {
    if (!Core::Signals::Instance()->DispatchAccessViolation(context, info->si_addr)) {
        std::printf("FAIL unhandled fault at %p\n", info->si_addr);
        std::_Exit(1);
    }
}
void InstallFaultHandler() {
    struct sigaction action {};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}
#endif

void SetPointer(u32* words, const void* pointer) {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    words[0] = static_cast<u32>(value);
    words[1] = static_cast<u32>(static_cast<u64>(value) >> 32);
}

} // namespace

int main() {
    using namespace Shader;
    InstallFaultHandler();

    // s[0:1] -> table: { pointer to the resources, one word of its own }
    // resources: four words (a sharp)
    alignas(16) u32 resources[4] = {11, 22, 33, 44};
    alignas(16) u32 table[3] = {};
    alignas(16) u32 user_data[NUM_USER_DATA_REGS] = {};
    SetPointer(table, resources);
    table[2] = 0x1234;
    SetPointer(user_data, table);

    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.user_data = std::span<const u32>(user_data);
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::Value root =
        ir.CompositeConstruct(ir.GetUserData(IR::ScalarReg::S0), ir.GetUserData(IR::ScalarReg::S1));
    const IR::U32 resources_lo = ir.ReadConst(root, ir.Imm32(0));
    const IR::U32 resources_hi = ir.ReadConst(root, ir.Imm32(1));
    const IR::U32 own_word = ir.ReadConst(root, ir.Imm32(2));
    const IR::Value pointer = ir.CompositeConstruct(resources_lo, resources_hi);
    IR::U32 sharp[4];
    for (u32 i = 0; i < 4; ++i) {
        sharp[i] = ir.ReadConst(pointer, ir.Imm32(i));
    }
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Optimization::FlattenExtendedUserdataPass(program);

    const auto walker = info.srt_info.walker_func;
    Expect(walker != nullptr, "the pass generated a walker");
    if (!walker) {
        return 1;
    }
    const u32 size = info.srt_info.flattened_bufsize_dw;
    const auto slot = [](const IR::U32& value) { return value.Inst()->Flags<u16>(); };
    std::vector<u32> flat(size);
    const auto walk = [&] {
        std::fill(flat.begin(), flat.end(), 0xAAAAAAAAu);
        std::memcpy(flat.data(), user_data, sizeof(user_data));
        walker(user_data, flat.data());
    };
    const auto reads_sharp = [&] {
        for (u32 i = 0; i < 4; ++i) {
            if (flat[slot(sharp[i])] != resources[i]) {
                return false;
            }
        }
        return flat[slot(own_word)] == 0x1234;
    };
    const auto sharp_zero = [&] {
        for (u32 i = 0; i < 4; ++i) {
            if (flat[slot(sharp[i])] != 0) {
                return false;
            }
        }
        return true;
    };

    walk();
    Expect(reads_sharp(), "a valid table: the sharp and the table's own word");

    SetPointer(table, nullptr);
    walk();
    Expect(sharp_zero() && flat[slot(own_word)] == 0x1234,
           "a null resource pointer: the sharp reads zero, the rest as it is");

    SetPointer(table, resources);
    walk();
    Expect(reads_sharp(), "the next draw with the pointer set reads the sharp again");

    SetPointer(user_data, nullptr);
    walk();
    Expect(sharp_zero() && flat[slot(own_word)] == 0, "a null root table: everything reads zero");

    SetPointer(user_data, table);
    walk();
    Expect(reads_sharp(), "the root set again reads everything again");

    // Not null, not mapped (the first 64 KiB never are): the fault handler's way.
    SetPointer(table, reinterpret_cast<const void*>(std::uintptr_t{0x100}));
    walk();
    Expect(sharp_zero() && flat[slot(own_word)] == 0x1234,
           "a bad resource pointer: the sharp reads zero in that draw");

    SetPointer(table, resources);
    walk();
    Expect(reads_sharp(), "after the fault the next draw reads the sharp again");

    std::printf("%s\n", failures ? "srt-walker-test FAILED" : "srt-walker-test ok");
    return failures ? 1 : 0;
}
