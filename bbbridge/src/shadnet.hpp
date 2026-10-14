// shadNet wire protocol: 15-byte packet framing, command / error / notification
// numbers, and the field numbers of the messages the bridge reads or writes.
// Sources: shadnet-p2p's protocol.h and shadnet.proto, the port's client.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace shadnet {

constexpr size_t kHeaderSize = 15;
constexpr uint32_t kMaxPacketSize = 0x800000;
constexpr uint32_t kProtocolVersion = 1;

enum PacketType : uint8_t { Request = 0, Reply = 1, Notification = 2, ServerInfo = 3 };

namespace cmd {
constexpr uint16_t Login = 0, Terminate = 1, AddFriend = 8, RemoveFriend = 9, AddBlock = 10,
                   RemoveBlock = 11, GetServerFeatures = 12, GetToken = 39, SetAppearOffline = 40,
                   ContextStart = 100, CreateRoom = 101, JoinRoom = 102, LeaveRoom = 103,
                   SearchRoom = 104, RequestSignalingInfos = 105, ContextStop = 106,
                   SetRoomDataInternal = 108, SetRoomDataExternal = 109, KickoutRoomMember = 110,
                   GetWorldInfoList = 111;
}

namespace err {
constexpr uint8_t NoError = 0, Malformed = 1, Invalid = 2, InvalidInput = 3, LoginError = 5,
                  LoginInvalidPassword = 8, RoomMissing = 14, NotFound = 26, Unsupported = 33;
}

namespace notify {
constexpr uint16_t RoomEvent = 10;
}

namespace room_event {
constexpr uint32_t MemberJoined = 0x1101, MemberLeft = 0x1102, KickedOut = 0x1103;
}

namespace cause {
constexpr uint32_t Leave = 1, Kickout = 2, ServerOperation = 4, MemberDisappeared = 5;
}

constexpr uint32_t kOwnerFlag = 0x80000000;   // SCE_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER

struct Header {
    uint8_t type = 0;
    uint16_t command = 0;
    uint32_t size = 0;
    uint64_t packet_id = 0;
};

inline bool parse_header(const std::string& buf, Header& h) {
    if (buf.size() < kHeaderSize) return false;
    const char* p = buf.data();
    h.type = static_cast<uint8_t>(p[0]);
    std::memcpy(&h.command, p + 1, 2);
    std::memcpy(&h.size, p + 3, 4);
    std::memcpy(&h.packet_id, p + 7, 8);
    return true;
}

inline std::string build_packet(uint8_t type, uint16_t command, uint64_t packet_id, const std::string& payload) {
    std::string out(kHeaderSize, '\0');
    const uint32_t size = static_cast<uint32_t>(kHeaderSize + payload.size());
    out[0] = static_cast<char>(type);
    std::memcpy(&out[1], &command, 2);
    std::memcpy(&out[3], &size, 4);
    std::memcpy(&out[7], &packet_id, 8);
    return out + payload;
}

inline std::string build_server_info() {
    std::string v(4, '\0');
    std::memcpy(v.data(), &kProtocolVersion, 4);
    return build_packet(ServerInfo, 0, 0, v);
}

inline std::string build_reply(uint16_t command, uint64_t packet_id, uint8_t error, const std::string& payload) {
    return build_packet(Reply, command, packet_id, std::string(1, static_cast<char>(error)) + payload);
}

// Every proto payload is framed as a u32-LE length + the message.
inline std::string proto_blob(const std::string& msg) {
    std::string out(4, '\0');
    const uint32_t n = static_cast<uint32_t>(msg.size());
    std::memcpy(out.data(), &n, 4);
    return out + msg;
}

inline bool parse_proto_blob(const std::string& payload, std::string& msg) {
    if (payload.size() < 4) return false;
    uint32_t n;
    std::memcpy(&n, payload.data(), 4);
    msg = payload.substr(4, n);
    return true;
}

inline std::string build_notification(uint16_t type, const std::string& msg) {
    return build_packet(Notification, type, 0, proto_blob(msg));
}

}  // namespace shadnet
