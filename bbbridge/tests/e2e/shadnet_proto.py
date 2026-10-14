"""shadNet wire protocol: 15-byte packet framing + message schemas.

Sources (read from the actual code, not the stale docs):
- Packet layout:   shadnet-p2p-main/documentation/protocol.md
- Command IDs:     shadnet-p2p-main/src/protocol.h  (== the port's client.h;
                   matching commands live at 100-115, NOT 12-23)
- Message fields:  the port's gpu/shadps4/shadnet/shadnet.proto (subset used
                   by the client) cross-checked against shadnet-p2p's fuller proto.
"""

import struct

import proto3

HEADER_SIZE = 15
MAX_PACKET_SIZE = 0x800000
PROTOCOL_VERSION = 1

PACKET_REQUEST = 0
PACKET_REPLY = 1
PACKET_NOTIFICATION = 2
PACKET_SERVER_INFO = 3


class Command:
    Login = 0
    Terminate = 1
    Create = 2
    Delete = 3
    AddFriend = 8
    RemoveFriend = 9
    AddBlock = 10
    RemoveBlock = 11
    GetServerFeatures = 12
    GetToken = 39
    SetAppearOffline = 40
    ContextStart = 100
    CreateRoom = 101
    JoinRoom = 102
    LeaveRoom = 103
    SearchRoom = 104
    RequestSignalingInfos = 105
    ContextStop = 106
    SetUserInfo = 107
    SetRoomDataInternal = 108
    SetRoomDataExternal = 109
    KickoutRoomMember = 110
    GetWorldInfoList = 111
    GetRoomDataExternalList = 112
    GetUserInfoList = 113
    GetRoomMemberDataExternalList = 114
    SendRoomMessage = 115
    GetBoardInfos = 30
    RecordScore = 31
    RecordScoreData = 32
    GetScoreData = 33
    GetScoreRange = 34
    GetScoreFriends = 35
    GetScoreNpid = 36


class Error:
    NoError = 0
    Malformed = 1
    Invalid = 2
    InvalidInput = 3
    TooSoon = 4
    LoginError = 5
    LoginAlreadyLoggedIn = 6
    LoginInvalidPassword = 8
    LoginInvalidToken = 9
    RoomMissing = 14
    RoomAlreadyJoined = 15
    RoomFull = 16
    Unauthorized = 23
    NotFound = 26
    Unsupported = 33


class Notify:
    FriendQuery = 5
    FriendNew = 6
    FriendLost = 7
    FriendStatus = 8
    RoomEvent = 10
    RoomMessage = 11


# Orbis room event ids (bbhost np_matching2.cpp + shadnet-p2p matching.md)
class RoomEvent:
    MemberJoined = 0x1101
    MemberLeft = 0x1102
    KickedOut = 0x1103
    RoomDestroyed = 0x1104
    RoomDataUpdatedInternal = 0x1106


class EventCause:
    Leave = 1
    Kickout = 2
    ServerOperation = 4
    MemberDisappeared = 5


# ---- protobuf message schemas (field number -> (name, kind)) ----------------

MATCHING_BIN_ATTR = {
    1: ("attr_id", "varint"),
    2: ("data", "bytes"),
    3: ("update_date", "varint"),
    4: ("update_member_id", "varint"),
}

MATCHING_INT_ATTR = {
    1: ("attr_id", "varint"),
    2: ("attr_value", "varint"),
}

MATCHING_ROOM_GROUP = {
    1: ("group_id", "varint"),
    2: ("has_passwd", "bool"),
    3: ("has_label", "bool"),
    4: ("label", "bytes"),
    5: ("slot_count", "varint"),
    6: ("num_members", "varint"),
}

MATCHING_ROOM_MEMBER_DATA = {
    1: ("npid", "string"),
    2: ("member_id", "varint"),
    3: ("team_id", "varint"),
    4: ("is_owner", "bool"),
    5: ("group", ("message", MATCHING_ROOM_GROUP)),
    6: ("bin_attrs_internal", ("repeated", ("message", MATCHING_BIN_ATTR))),
    7: ("join_date", "varint"),
    8: ("nat_type", "varint"),
    9: ("flag_attr", "varint"),
    10: ("addr", "string"),
    11: ("port", "varint"),
    12: ("group_id", "varint"),
    13: ("account_id", "varint"),
    14: ("platform", "varint"),
}

MATCHING_ROOM_DATA_INTERNAL = {
    1: ("public_slots", "varint"),
    2: ("private_slots", "varint"),
    3: ("open_public_slots", "varint"),
    4: ("open_private_slots", "varint"),
    5: ("max_slot", "varint"),
    6: ("server_id", "varint"),
    7: ("world_id", "varint"),
    8: ("lobby_id", "varint"),
    9: ("room_id", "varint"),
    10: ("passwd_slot_mask", "varint"),
    11: ("joined_slot_mask", "varint"),
    12: ("groups", ("repeated", ("message", MATCHING_ROOM_GROUP))),
    13: ("flags", "varint"),
    14: ("bin_attrs_internal", ("repeated", ("message", MATCHING_BIN_ATTR))),
}

CREATE_JOIN_ROOM_RESPONSE = {
    1: ("room_data", ("message", MATCHING_ROOM_DATA_INTERNAL)),
    2: ("members", ("repeated", ("message", MATCHING_ROOM_MEMBER_DATA))),
    3: ("me_member_id", "varint"),
    4: ("owner_member_id", "varint"),
}

MATCHING_WORLD = {
    1: ("world_id", "varint"),
    2: ("lobbies_num", "varint"),
    3: ("max_lobby_members", "varint"),
    4: ("lobby_members_num", "varint"),
    5: ("rooms_num", "varint"),
    6: ("room_members_num", "varint"),
}

LOGIN_REQUEST = {
    1: ("npid", "string"),
    2: ("password", "string"),
    3: ("token", "string"),
    4: ("title_id", "string"),
    5: ("title_name", "string"),
    6: ("appear_offline", "bool"),
}

LOGIN_REPLY = {
    1: ("avatar_url", "string"),
    2: ("user_id", "varint"),
    3: ("friends", ("repeated", ("message", {
        1: ("npid", "string"),
        2: ("online", "bool"),
        3: ("presence", "bytes"),
    }))),
    4: ("friend_requests_sent", ("repeated", "string")),
    5: ("friend_requests_received", ("repeated", "string")),
    6: ("blocked", ("repeated", "string")),
}

SERVER_FEATURES_REPLY = {1: ("matching2_enabled", "bool")}

GET_TOKEN_REPLY = {
    1: ("token", "string"),
    2: ("user_id", "varint"),
    3: ("npid", "string"),
}

CONTEXT_START_REQUEST = {1: ("ctx_id", "varint")}
CONTEXT_STOP_REQUEST = {1: ("ctx_id", "varint")}

CREATE_ROOM_REQUEST = {
    1: ("req_id", "varint"),
    2: ("max_slots", "varint"),
    3: ("team_id", "varint"),
    4: ("world_id", "varint"),
    5: ("lobby_id", "varint"),
    6: ("flags", "varint"),
    7: ("group_config_count", "varint"),
    8: ("allowed_user_count", "varint"),
    9: ("blocked_user_count", "varint"),
    10: ("internal_bin_attr_count", "varint"),
    11: ("external_search_int_attrs", ("repeated", ("message", MATCHING_INT_ATTR))),
    12: ("external_search_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    13: ("external_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    14: ("member_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    15: ("join_group_label_present", "bool"),
    16: ("room_password_present", "bool"),
    17: ("sig_type", "varint"),
    18: ("sig_flag", "varint"),
    19: ("sig_main_member", "varint"),
    20: ("internal_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    21: ("user_id_kind", "varint"),
    22: ("allowed_online_ids", ("repeated", "string")),
    23: ("blocked_online_ids", ("repeated", "string")),
    24: ("allowed_account_ids", ("repeated", "varint")),
    25: ("blocked_account_ids", ("repeated", "varint")),
    26: ("group_configs", ("repeated", ("message", {
        1: ("slot_count", "varint"),
        2: ("has_label", "bool"),
        3: ("label", "bytes"),
        4: ("has_passwd", "bool"),
    }))),
    27: ("room_password", "bytes"),
    28: ("has_passwd_slot_mask", "bool"),
    29: ("passwd_slot_mask", "varint"),
    30: ("join_group_label", "bytes"),
}

CREATE_ROOM_REPLY = {
    1: ("room_id", "varint"),
    2: ("server_id", "varint"),
    3: ("world_id", "varint"),
    4: ("lobby_id", "varint"),
    5: ("member_id", "varint"),
    6: ("max_slots", "varint"),
    7: ("flags", "varint"),
    8: ("cur_members", "varint"),
    9: ("details", ("message", CREATE_JOIN_ROOM_RESPONSE)),
}

JOIN_ROOM_REQUEST = {
    1: ("room_id", "varint"),
    2: ("req_id", "varint"),
    3: ("team_id", "varint"),
    4: ("join_flags", "varint"),
    5: ("blocked_user_count", "varint"),
    6: ("member_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    7: ("room_password_present", "bool"),
    8: ("join_group_label_present", "bool"),
    9: ("user_id_kind", "varint"),
    10: ("blocked_online_ids", ("repeated", "string")),
    11: ("blocked_account_ids", ("repeated", "varint")),
}

JOIN_ROOM_REPLY = {
    1: ("room_id", "varint"),
    2: ("member_id", "varint"),
    3: ("max_slots", "varint"),
    4: ("flags", "varint"),
    5: ("cur_members", "varint"),
    6: ("details", ("message", CREATE_JOIN_ROOM_RESPONSE)),
}

LEAVE_ROOM_REQUEST = {1: ("room_id", "varint"), 2: ("req_id", "varint")}
LEAVE_ROOM_REPLY = {1: ("room_id", "varint")}

REQUEST_SIGNALING_INFOS_REQUEST = {1: ("target_npid", "string")}
REQUEST_SIGNALING_INFOS_REPLY = {
    1: ("target_npid", "string"),
    2: ("target_ip", "string"),
    3: ("target_port", "varint"),
    4: ("target_member_id", "varint"),
}

SET_ROOM_DATA_INTERNAL_REQUEST = {
    1: ("req_id", "varint"),
    2: ("room_id", "varint"),
    3: ("flag_filter", "varint"),
    4: ("flag_attr", "varint"),
    5: ("bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    6: ("has_passwd_mask", "bool"),
    7: ("passwd_slot_mask", "varint"),
}
SET_ROOM_DATA_INTERNAL_REPLY = {1: ("room_id", "varint")}

SET_ROOM_DATA_EXTERNAL_REQUEST = {
    1: ("req_id", "varint"),
    2: ("room_id", "varint"),
    3: ("search_int_attrs", ("repeated", ("message", MATCHING_INT_ATTR))),
    4: ("search_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    5: ("ext_bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
}
SET_ROOM_DATA_EXTERNAL_REPLY = {1: ("room_id", "varint")}

KICKOUT_ROOM_MEMBER_REQUEST = {
    1: ("room_id", "varint"),
    2: ("req_id", "varint"),
    3: ("target_member_id", "varint"),
    4: ("block_kick_flag", "varint"),
}
KICKOUT_ROOM_MEMBER_REPLY = {1: ("room_id", "varint")}

SEARCH_ROOM_REPLY = {
    1: ("rooms", ("repeated", ("message", {
        1: ("max_slot", "varint"),
        2: ("cur_members", "varint"),
        3: ("flags", "varint"),
        4: ("server_id", "varint"),
        5: ("world_id", "varint"),
        6: ("lobby_id", "varint"),
        7: ("room_id", "varint"),
    }))),
    2: ("range_start", "varint"),
    3: ("range_total", "varint"),
    4: ("range_result", "varint"),
}

GET_WORLD_INFO_LIST_REQUEST = {1: ("server_id", "varint")}
GET_WORLD_INFO_LIST_REPLY = {1: ("worlds", ("repeated", ("message", MATCHING_WORLD)))}

FRIEND_COMMAND_REQUEST = {1: ("npid", "string")}

NOTIFY_ROOM_EVENT = {
    1: ("ctx_id", "varint"),
    2: ("room_id", "varint"),
    3: ("event", "varint"),
    4: ("event_cause", "varint"),
    5: ("error_code", "varint"),
    6: ("member", ("message", MATCHING_ROOM_MEMBER_DATA)),
    7: ("bin_attrs", ("repeated", ("message", MATCHING_BIN_ATTR))),
    8: ("flags", "varint"),
    9: ("has_passwd_mask", "bool"),
    10: ("passwd_slot_mask", "varint"),
}

SET_APPEAR_OFFLINE_REQUEST = {1: ("appear_offline", "bool")}


# ---- packet framing ---------------------------------------------------------

def build_packet(packet_type, command, packet_id, payload=b""):
    size = HEADER_SIZE + len(payload)
    return struct.pack("<BHIQ", packet_type, command, size, packet_id) + payload


def build_server_info():
    return build_packet(PACKET_SERVER_INFO, 0, 0, struct.pack("<I", PROTOCOL_VERSION))


def build_reply(command, packet_id, error, payload=b""):
    return build_packet(PACKET_REPLY, command, packet_id, bytes([error]) + payload)


def build_notification(notification_type, payload):
    return build_packet(PACKET_NOTIFICATION, notification_type, 0, payload)


def proto_blob(message_bytes):
    """The u32-LE length prefix + message framing every proto payload uses."""
    return struct.pack("<I", len(message_bytes)) + message_bytes


def parse_proto_blob(payload):
    if len(payload) < 4:
        return None
    (size,) = struct.unpack_from("<I", payload, 0)
    return payload[4:4 + size]


def parse_header(buf):
    if len(buf) < HEADER_SIZE:
        return None
    packet_type, command, size, packet_id = struct.unpack_from("<BHIQ", buf, 0)
    return packet_type, command, size, packet_id
