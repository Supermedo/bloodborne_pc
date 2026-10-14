"""Minimal protobuf3 wire-format codec (no dependencies).

Only what shadnet_proto.py needs: varint and length-delimited fields.
proto3 semantics: fields at their default value are omitted on encode;
decode returns only the fields present on the wire.

Schema entry:  field_number -> (name, kind)
    kind: "varint" (int), "bool", "string", "bytes", or ("message", schema).
A wire field may repeat; decoded values are then lists (never for our
scalars in practice, but decode keeps the last occurrence for scalars and
extends a list for messages, which is what the shadnet messages use).
"""

import struct


def encode_varint(value):
    if value < 0:
        # int32/int64 negatives are encoded as 10-byte two's complement
        value &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def decode_varint(buf, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("varint runs past end of buffer")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
        if shift > 70:
            raise ValueError("varint too long")


def encode_tag(field_number, wire_type):
    return encode_varint((field_number << 3) | wire_type)


def encode_field(field_number, kind, value):
    """Encode one field occurrence. Returns bytes."""
    if kind == "varint":
        if value in (0, None):
            return b""
        return encode_tag(field_number, 0) + encode_varint(int(value))
    if kind == "bool":
        if not value:
            return b""
        return encode_tag(field_number, 0) + encode_varint(1)
    if kind in ("string", "bytes"):
        if not value:
            return b""
        data = value.encode("utf-8") if kind == "string" else bytes(value)
        return encode_tag(field_number, 2) + encode_varint(len(data)) + data
    if isinstance(kind, tuple) and kind[0] == "message":
        if value is None:
            return b""
        data = encode_message(kind[1], value)
        return encode_tag(field_number, 2) + encode_varint(len(data)) + data
    raise ValueError("unknown kind %r" % (kind,))


def encode_message(schema, values):
    """values: {field_number: value}. Repeated messages take a list."""
    out = bytearray()
    for field_number in sorted(values):
        name, kind = schema[field_number]
        value = values[field_number]
        if isinstance(kind, tuple) and kind[0] == "repeated":
            _, inner = kind
            for item in (value or []):
                out += encode_field(field_number, inner, item)
        else:
            out += encode_field(field_number, kind, value)
    return bytes(out)


def decode_raw_fields(buf):
    """Yield (field_number, wire_type, value) for every field on the wire.
    Length-delimited values are bytes; varints are ints."""
    pos = 0
    n = len(buf)
    while pos < n:
        tag, pos = decode_varint(buf, pos)
        field_number = tag >> 3
        wire_type = tag & 7
        if wire_type == 0:
            value, pos = decode_varint(buf, pos)
        elif wire_type == 2:
            length, pos = decode_varint(buf, pos)
            value = buf[pos:pos + length]
            if len(value) != length:
                raise ValueError("length-delimited field runs past end")
            pos += length
        elif wire_type == 5:  # fixed32
            value = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
        elif wire_type == 1:  # fixed64
            value = struct.unpack_from("<Q", buf, pos)[0]
            pos += 8
        else:
            raise ValueError("unsupported wire type %d" % wire_type)
        yield field_number, wire_type, value


def decode_message(schema, buf):
    """Decode into {name: value}. Scalars keep the last occurrence; repeated
    messages accumulate into lists; nested messages are decoded recursively.
    Unknown fields are kept under name "<field:N>" as raw bytes/int."""
    out = {}
    for field_number, wire_type, value in decode_raw_fields(buf):
        entry = schema.get(field_number)
        if entry is None:
            name = "<field:%d>" % field_number
            out[name] = value
            continue
        name, kind = entry
        if isinstance(kind, tuple) and kind[0] == "repeated":
            _, inner = kind
            if isinstance(inner, tuple):
                item = decode_message(inner[1], value)
            elif inner == "string":
                item = value.decode("utf-8", "replace")
            else:
                item = value
            out.setdefault(name, []).append(item)
        elif isinstance(kind, tuple) and kind[0] == "message":
            out[name] = decode_message(kind[1], value)
        elif kind == "string":
            out[name] = value.decode("utf-8", "replace") if isinstance(value, (bytes, bytearray)) else value
        elif kind == "bool":
            out[name] = bool(value)
        else:  # varint
            out[name] = value
    return out
