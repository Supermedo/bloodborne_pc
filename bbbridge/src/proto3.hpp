// Minimal protobuf3 wire format: varint and length-delimited fields, which is
// all the shadNet messages use. proto3 semantics: fields at their default value
// are omitted on encode; decode keeps what is on the wire.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb {

class Writer {
public:
    void varint(uint64_t v) {
        while (v >= 0x80) {
            buf_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        buf_.push_back(static_cast<char>(v));
    }
    void tag(int field, int wire) { varint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(wire)); }

    Writer& u(int field, uint64_t v) {          // uint / int (0 omitted)
        if (v) { tag(field, 0); varint(v); }
        return *this;
    }
    Writer& i(int field, int64_t v) { return u(field, static_cast<uint64_t>(v)); }
    Writer& flag(int field, bool v) {           // bool (false omitted)
        if (v) { tag(field, 0); varint(1); }
        return *this;
    }
    Writer& s(int field, const std::string& v) {  // string / bytes ("" omitted)
        if (!v.empty()) { tag(field, 2); varint(v.size()); buf_ += v; }
        return *this;
    }
    Writer& msg(int field, const std::string& encoded) {  // embedded message, always written
        tag(field, 2);
        varint(encoded.size());
        buf_ += encoded;
        return *this;
    }
    const std::string& str() const { return buf_; }

private:
    std::string buf_;
};

struct Field {
    int number = 0;
    int wire = 0;
    uint64_t value = 0;     // varint / fixed
    std::string data;       // length-delimited
};

bool parse(const std::string& buf, std::vector<Field>& out);

// Read access by field number; scalars keep the last occurrence.
class Message {
public:
    Message() = default;
    explicit Message(const std::string& buf) { ok_ = parse(buf, fields_); }
    bool ok() const { return ok_; }
    uint64_t u(int field, uint64_t def = 0) const;
    std::string s(int field) const;
    bool has(int field) const;
    std::vector<std::string> all(int field) const;

private:
    std::vector<Field> fields_;
    bool ok_ = false;
};

}  // namespace pb
