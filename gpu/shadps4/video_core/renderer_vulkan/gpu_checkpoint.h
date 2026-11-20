#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan::Checkpoint {

enum class Op : uint32_t {
    None,
    GameDraw,
    GameDrawIndirect,
    GameDispatch,
    GameDispatchIndirect,
    RtPass,
    RtPassDone,
    Dlss,
    DlssRr,
    DlssDone,
    ProxyFill,
    ProxyResolve,
    RrGuides,
    RrLinearize,
    RrTonemap,
    Count,
};

constexpr const char* OpName(uint32_t op) {
    switch (Op(op)) {
    case Op::None:
        return "none";
    case Op::GameDraw:
        return "game draw (shader hash)";
    case Op::GameDrawIndirect:
        return "game indirect draw (shader hash)";
    case Op::GameDispatch:
        return "game dispatch (cs hash)";
    case Op::GameDispatchIndirect:
        return "game indirect dispatch (cs hash)";
    case Op::RtPass:
        return "ray tracing pass start (crumb id)";
    case Op::RtPassDone:
        return "ray tracing pass end (crumb id)";
    case Op::Dlss:
        return "DLSS evaluate start";
    case Op::DlssRr:
        return "DLSS ray reconstruction evaluate start";
    case Op::DlssDone:
        return "DLSS evaluate end";
    case Op::ProxyFill:
        return "scene proxy fill copy (image id)";
    case Op::ProxyResolve:
        return "scene proxy resolve copy (image id)";
    case Op::RrGuides:
        return "DLSS RR guide pass";
    case Op::RrLinearize:
        return "DLSS RR linearize pass";
    case Op::RrTonemap:
        return "DLSS RR tonemap pass";
    case Op::Count:
        break;
    }
    return "unknown";
}

struct Mark {
    uint32_t op{};
    uint32_t frame{};
    uint64_t hash{};
    uint32_t serial{};
    uint32_t pad{};
};
static_assert(sizeof(Mark) == 24);

constexpr uint32_t RingSize = 1u << 14;

struct Ring {
    std::array<Mark, RingSize> marks{};
    std::atomic<uint32_t> next{1};

    const Mark* Push(Op op, uint64_t hash, uint32_t frame) {
        const uint32_t serial = next.fetch_add(1, std::memory_order_relaxed);
        Mark& m = marks[serial % RingSize];
        m.op = uint32_t(op);
        m.frame = frame;
        m.hash = hash;
        m.serial = serial;
        return &m;
    }

    const Mark* At(uint32_t serial) const {
        const Mark& m = marks[serial % RingSize];
        return m.serial == serial && serial != 0 ? &m : nullptr;
    }

    bool Owns(const void* marker) const {
        const auto* p = static_cast<const Mark*>(marker);
        if (p < marks.data() || p >= marks.data() + RingSize) {
            return false;
        }
        const size_t offset = size_t(reinterpret_cast<const char*>(p) -
                                     reinterpret_cast<const char*>(marks.data()));
        return offset % sizeof(Mark) == 0 && p->op < uint32_t(Op::Count);
    }

    const Mark* Last() const {
        return At(next.load(std::memory_order_relaxed) - 1);
    }
};

inline Ring ring;
inline std::atomic<bool> enabled{false};
inline std::atomic<uint32_t> frame{0};

template <class CmdBuf>
void Set(CmdBuf cmdbuf, Op op, uint64_t hash) {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    cmdbuf.setCheckpointNV(ring.Push(op, hash, frame.load(std::memory_order_relaxed)));
}

inline std::string DescribeMark(const Mark& m) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s %#llx (frame %u, op #%u)", OpName(m.op),
                  (unsigned long long)m.hash, m.frame, m.serial);
    return line;
}

inline std::string DescribeCheckpoint(const char* stage, const Ring& r, const void* marker) {
    if (!marker) {
        return std::string("GPU: checkpoint ") + stage + ": no marker";
    }
    if (!r.Owns(marker)) {
        char line[96];
        std::snprintf(line, sizeof(line), ": foreign marker %p (set inside DLSS/NGX or the driver)",
                      marker);
        return std::string("GPU: checkpoint ") + stage + line;
    }
    return std::string("GPU: checkpoint ") + stage + ": " +
           DescribeMark(*static_cast<const Mark*>(marker));
}

inline std::string DescribeWindow(const Ring& r, uint32_t done_serial, uint32_t started_serial,
                                  uint32_t max_lines) {
    std::string out;
    if (started_serial == 0 || started_serial < done_serial) {
        return out;
    }
    uint32_t first = done_serial;
    if (started_serial - first + 1 > max_lines) {
        first = started_serial - max_lines + 1;
    }
    for (uint32_t s = first; s <= started_serial; ++s) {
        const Mark* m = r.At(s);
        if (!m) {
            continue;
        }
        out += s == done_serial ? "GPU:   done     " : "GPU:   pending  ";
        out += DescribeMark(*m);
        out += '\n';
    }
    return out;
}

inline std::string DescribeForeign(const Ring& r, uint32_t max_lines) {
    const Mark* last = r.Last();
    std::string out = "GPU: the GPU stopped inside or right after a DLSS/NGX pass; last own "
                      "marker recorded: ";
    out += last ? DescribeMark(*last) : std::string("none");
    out += '\n';
    if (last) {
        const uint32_t first = last->serial > max_lines ? last->serial - max_lines + 1 : 1;
        out += DescribeWindow(r, first, last->serial, max_lines);
    }
    return out;
}

}
