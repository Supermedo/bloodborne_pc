#include "proto3.hpp"

#include <cstring>

namespace pb {

namespace {
bool read_varint(const std::string& b, size_t& pos, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (true) {
        if (pos >= b.size() || shift > 63) return false;
        const uint8_t c = static_cast<uint8_t>(b[pos++]);
        out |= static_cast<uint64_t>(c & 0x7F) << shift;
        if (!(c & 0x80)) return true;
        shift += 7;
    }
}
}  // namespace

bool parse(const std::string& b, std::vector<Field>& out) {
    size_t pos = 0;
    while (pos < b.size()) {
        uint64_t tag = 0;
        if (!read_varint(b, pos, tag)) return false;
        Field f;
        f.number = static_cast<int>(tag >> 3);
        f.wire = static_cast<int>(tag & 7);
        switch (f.wire) {
        case 0:
            if (!read_varint(b, pos, f.value)) return false;
            break;
        case 2: {
            uint64_t len = 0;
            if (!read_varint(b, pos, len) || len > b.size() - pos) return false;
            f.data.assign(b, pos, static_cast<size_t>(len));
            pos += static_cast<size_t>(len);
            break;
        }
        case 5:
            if (b.size() - pos < 4) return false;
            { uint32_t v; std::memcpy(&v, b.data() + pos, 4); f.value = v; }
            pos += 4;
            break;
        case 1:
            if (b.size() - pos < 8) return false;
            std::memcpy(&f.value, b.data() + pos, 8);
            pos += 8;
            break;
        default:
            return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

uint64_t Message::u(int field, uint64_t def) const {
    uint64_t v = def;
    for (const auto& f : fields_)
        if (f.number == field && f.wire != 2) v = f.value;
    return v;
}

std::string Message::s(int field) const {
    std::string v;
    for (const auto& f : fields_)
        if (f.number == field && f.wire == 2) v = f.data;
    return v;
}

bool Message::has(int field) const {
    for (const auto& f : fields_)
        if (f.number == field) return true;
    return false;
}

std::vector<std::string> Message::all(int field) const {
    std::vector<std::string> v;
    for (const auto& f : fields_)
        if (f.number == field && f.wire == 2) v.push_back(f.data);
    return v;
}

}  // namespace pb
