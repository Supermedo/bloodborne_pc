#include "bridge.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>

#include "proto3.hpp"
#include "shadnet.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace bb {

namespace sn = shadnet;
namespace fs = std::filesystem;

namespace {

// bbhost's server drops a member whose heartbeat is 15 s old; bbhost sends one
// every 5 s (src/hle/np_matching2.cpp heartbeat_loop).
constexpr double kHeartbeatInterval = 5.0;

const std::set<std::string> kHopByHop = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
                                         "te", "trailers", "transfer-encoding", "upgrade", "host",
                                         "content-length", "accept-encoding"};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string repr(const std::string& s) { return "'" + s + "'"; }

std::string sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out(32, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                   static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(out.data()), 32);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

bool read_exact(SOCKET s, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const int r = recv(s, buf + got, static_cast<int>(std::min<size_t>(n - got, 1 << 20)), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

}  // namespace

uint64_t account_id_for(const std::string& npid) {
    // Unique per player (a fixed 1 made every bridge player the same account to
    // the game server), stable per npid, and above 2^52 where real shadNet's
    // sequential ids do not reach. Same value as the Python bridge.
    const std::string d = sha256("bbbridge:" + npid);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(d[i]);
    return (1ull << 52) | (v & ((1ull << 48) - 1));
}

// ---- config ----

Config Config::from_json(const json& j) {
    Config c;
    c.bbhost_server = jstr(j, "bbhost_server", c.bbhost_server);
    c.community_server = jstr(j, "community_server", c.community_server);
    c.auth_server = jstr(j, "auth_server", c.auth_server);
    c.bind_host = jstr(j, "bind_host", c.bind_host);
    c.tcp_port = static_cast<int>(jint(j, "tcp_port", c.tcp_port));
    c.udp_port = static_cast<int>(jint(j, "udp_port", c.udp_port));
    c.webapi_port = static_cast<int>(jint(j, "webapi_port", c.webapi_port));
    c.p2p_gateway = jint(j, "p2p_gateway", 1) != 0;
    c.p2p_port = static_cast<int>(jint(j, "p2p_port", 0));
    c.port_p2p_port = static_cast<int>(jint(j, "port_p2p_port", c.port_p2p_port));
    c.stun_server = jstr(j, "stun_server", "");
    c.local_addr = jstr(j, "local_addr", "");
    if (jhas(j, "poll_interval") && j["poll_interval"].is_number()) c.poll_interval = j["poll_interval"].get<double>();
    c.state_file = jstr(j, "state_file", c.state_file);
    c.trace = jint(j, "trace", 0) != 0;
    c.debug_endpoints = jint(j, "debug_endpoints", 0) != 0;
    c.create_accounts = jint(j, "create_accounts", 0) != 0;
    if (jhas(j, "stun_refresh") && j["stun_refresh"].is_number()) c.stun_refresh = j["stun_refresh"].get<double>();
    c.user_agent = jstr(j, "user_agent", c.user_agent);
    if (j.is_object() && j.contains("game_proxy_routes") && j["game_proxy_routes"].is_object()) {
        c.game_proxy_routes.clear();
        for (auto it = j["game_proxy_routes"].begin(); it != j["game_proxy_routes"].end(); ++it)
            c.game_proxy_routes[std::stoi(it.key())] = it.value().get<std::string>();
    }
    return c;
}

std::optional<std::pair<std::string, int>> Config::stun_endpoint() const {
    std::string spec = stun_server;
    if (spec == "off") return std::nullopt;
    if (spec.empty()) {
        std::string host = bbhost_server;
        const size_t s = host.find("://");
        if (s != std::string::npos) host = host.substr(s + 3);
        host = host.substr(0, host.find_first_of(":/"));
        if (host.empty()) return std::nullopt;
        return std::make_pair(host, 3478);
    }
    const size_t c = spec.find(':');
    if (c == std::string::npos) return std::make_pair(spec, 3478);
    return std::make_pair(spec.substr(0, c), std::stoi(spec.substr(c + 1)));
}

// ---- tcp connection ----

bool TcpConn::send(const std::string& data) {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (!alive) return false;
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            alive = false;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void TcpConn::close() {
    alive = false;
    shutdown(sock, SD_BOTH);
}

// ---- session ----

Session::Session(Bridge& bridge, std::shared_ptr<TcpConn> c)
    : bb(bridge.cfg.bbhost_server), conn(std::move(c)), bridge_(bridge) {
    bb.user_agent = bridge.cfg.user_agent;
}

Session::~Session() {
    {
        std::lock_guard<std::mutex> lk(pump_mu_);
        pump_stop_ = true;
    }
    pump_cv_.notify_all();
    std::lock_guard<std::mutex> lk(join_mu_);
    if (pump_.joinable()) {
        if (pump_.get_id() == std::this_thread::get_id()) pump_.detach();
        else pump_.join();
    }
}

std::string Session::npid() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return npid_;
}

uint64_t Session::room_id() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return room_.room_id;
}

int Session::room_members() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return static_cast<int>(room_.members.size());
}

void Session::set_advertisement(const json& adv) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    advertisement_ = adv;
}

void Session::on_mapped_change(const Endpoint& mapped) {
    std::string online, session_id;
    int member = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (!logged_in_ || bb.token().empty()) return;
        online = bb.online_id().empty() ? npid_ : bb.online_id();
        session_id = room_.session_id;
        member = room_.member_id;
    }
    bb.signaling_update(online, mapped, session_id, member);
}

std::pair<uint8_t, std::string> Session::handle(uint16_t command, uint64_t packet_id, const std::string& payload) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Reply r;
    try {
        switch (command) {
        case sn::cmd::Login: r = on_login(payload); break;
        case sn::cmd::Terminate:
            terminate_requested = true;
            r = {sn::err::NoError, ""};
            break;
        case sn::cmd::GetServerFeatures:
            r = {sn::err::NoError, sn::proto_blob(pb::Writer().flag(1, true).str())};
            break;
        case sn::cmd::GetToken: r = on_get_token(); break;
        case sn::cmd::SetAppearOffline:
        case sn::cmd::AddFriend:
        case sn::cmd::RemoveFriend:
        case sn::cmd::AddBlock:
        case sn::cmd::RemoveBlock: r = {sn::err::NoError, ""}; break;   // no friend lists across the bridge
        case sn::cmd::ContextStart: r = on_context_start(payload); break;
        case sn::cmd::ContextStop: r = on_context_stop(); break;
        case sn::cmd::GetWorldInfoList: r = on_get_world_info_list(); break;
        case sn::cmd::CreateRoom: r = on_create_room(payload); break;
        case sn::cmd::JoinRoom: r = on_join_room(payload); break;
        case sn::cmd::LeaveRoom: r = on_leave_room(payload); break;
        case sn::cmd::SearchRoom: r = {sn::err::NoError, sn::proto_blob("")}; break;
        case sn::cmd::RequestSignalingInfos: r = on_request_signaling_infos(payload); break;
        case sn::cmd::SetRoomDataInternal:
        case sn::cmd::SetRoomDataExternal: r = on_room_id_echo(payload); break;
        case sn::cmd::KickoutRoomMember: r = on_kickout(payload); break;
        default:
            log(fmt("tcp: unhandled command %d (replying Unsupported)", command));
            return {sn::err::Unsupported, ""};
        }
    } catch (const BridgeError& e) {
        log(fmt("command %d failed: %s", command, e.what()));
        return {sn::err::Invalid, ""};
    }
    if (bridge_.cfg.trace)
        log(fmt("cmd %d pkt %llu -> error %d, %zu bytes", command, static_cast<unsigned long long>(packet_id), r.first,
                r.second.size()));
    return r;
}

Session::Reply Session::on_login(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    pb::Message req(blob);
    npid_ = req.s(1);
    npid_.erase(0, npid_.find_first_not_of(" \t"));
    npid_.erase(npid_.find_last_not_of(" \t") + 1);
    title_id_ = req.s(4);
    if (npid_.empty()) return {sn::err::LoginInvalidPassword, ""};
    log(fmt("login: npid=%s title=%s", repr(npid_).c_str(), title_id_.empty() ? "?" : title_id_.c_str()));
    if (!bbhost_account()) return {sn::err::LoginError, ""};
    logged_in_ = true;
    account_id_ = account_id_for(npid_);
    log(fmt("login: account id %llu for %s", static_cast<unsigned long long>(account_id_), repr(npid_).c_str()));
    return {sn::err::NoError, sn::proto_blob(pb::Writer().u(2, account_id_).str())};
}

bool Session::bbhost_account() {
    // Sign in to the bbhost account named like the npid: the stored token, else
    // the recovery code, else a new account. /auth/recover rotates the recovery
    // code on every call, and one whose answer is lost (a timeout during a server
    // restart) leaves the account's code unknown - so recover only when the
    // server refuses the stored token.
    json state = bridge_.load_state();
    if (!state.is_object()) state = json::object();
    json& accounts = state["accounts"];
    if (!accounts.is_object()) accounts = json::object();
    json entry = accounts.contains(npid_) && accounts[npid_].is_object() ? accounts[npid_] : json::object();
    const std::string stored_name = jstr(entry, "bbhost_name", npid_).empty() ? npid_ : jstr(entry, "bbhost_name", npid_);
    try {
        if (!jstr(entry, "token").empty()) {
            const std::string online = jstr(entry, "online_id", stored_name);
            bb.set_identity(jstr(entry, "token"), online.empty() ? stored_name : online);
            const std::optional<bool> verdict = bb.token_valid(bb.online_id());
            if (verdict != false) {
                log(fmt("bbhost: using the stored token for %s%s", repr(stored_name).c_str(),
                        verdict ? "" : " (server unreachable, kept)"));
                return true;
            }
            if (jstr(entry, "linked") == "discord") {   // no recovery code: only a new sign-in brings it back
                log(fmt("bbhost: the Discord sign-in for %s has expired - use 'Sign in with Discord' in bbbridge "
                        "again.", repr(stored_name).c_str()));
                return false;
            }
            log(fmt("bbhost: stored token for %s refused; recovering", repr(stored_name).c_str()));
            bb.set_identity("", "");
        }
        if (!jstr(entry, "recovery_code").empty()) {
            std::string code;
            try {
                code = bb.account_recover(stored_name, jstr(entry, "recovery_code"));
            } catch (const BridgeError& e) {
                if (std::string(e.what()).find("refused") != std::string::npos)
                    log(fmt("bbhost: the recovery code for %s no longer works - the account cannot be recovered "
                            "from here. Pick a new online id in the launcher (or ask the server owner).",
                            repr(stored_name).c_str()));
                throw;
            }
            accounts[npid_] = {{"bbhost_name", stored_name},
                               {"recovery_code", code.empty() ? jstr(entry, "recovery_code") : code},
                               {"token", bb.token()},
                               {"online_id", bb.online_id()}};
            bridge_.save_state(state);
            log(fmt("bbhost: recovered account %s", repr(stored_name).c_str()));
            return true;
        }
        // Players sign in with Discord in the app, which stores the account under
        // its name and makes that name the NPID. No account here: say so, and
        // do not invent one.
        if (!bridge_.cfg.create_accounts) {
            log(fmt("bbhost: no account for %s on this PC - use 'Sign in with Discord' in bbbridge first.",
                    repr(npid_).c_str()));
            return false;
        }
        // The account name must be the online id: the game signs in to the game
        // server under it, and the token's account should match.
        std::string code;
        try {
            code = bb.account_create(npid_);
        } catch (const BridgeError& e) {
            std::string what = lower(e.what());
            if (what.find("taken") != std::string::npos)
                log(fmt("bbhost: the name %s is already taken on The Hunter's Dream. Pick another online id "
                        "(NPID) in the bbport launcher.", repr(npid_).c_str()));
            throw;
        }
        accounts[npid_] = {{"bbhost_name", npid_}, {"recovery_code", code}, {"token", bb.token()},
                           {"online_id", bb.online_id()}};
        bridge_.save_state(state);
        log(fmt("bbhost: created account %s (token and recovery code stored in bbbridge_state.json - keep that "
                "file)", repr(npid_).c_str()));
        return true;
    } catch (const BridgeError& e) {
        log(fmt("bbhost account failed for %s: %s", repr(npid_).c_str(), e.what()));
        return false;
    }
}

Session::Reply Session::on_get_token() {
    pb::Writer w;
    w.s(1, bb.token()).u(2, account_id_ ? account_id_ : account_id_for(npid_)).s(3, npid_);
    return {sn::err::NoError, sn::proto_blob(w.str())};
}

std::pair<Endpoint, Endpoint> Session::p2p_addresses() {
    if (bridge_.gateway) return bridge_.gateway->endpoints();
    const std::string local_ip = bridge_.cfg.local_addr.empty() ? detect_lan_addr() : bridge_.cfg.local_addr;
    const std::optional<Endpoint> ext = bridge_.registry_get(npid_);
    const Endpoint local{local_ip, ext ? ext->port : 9307};
    const Endpoint mapped{ext ? ext->ip : local_ip, local.port};
    return {local, mapped};
}

Session::Reply Session::on_context_start(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    ctx_id_ = pb::Message(blob).u(1);
    if (!bridge_.registry_get(npid_))
        log(fmt("warning: no STUN ping seen for %s yet; P2P endpoint unknown", repr(npid_).c_str()));
    const auto [local, mapped] = p2p_addresses();
    try {
        bb.context_start(bb.online_id().empty() ? npid_ : bb.online_id(), local, mapped);
    } catch (const BridgeError& e) {
        log(std::string("context_start failed: ") + e.what());
        return {sn::err::Invalid, ""};
    }
    start_pump();
    return {sn::err::NoError, ""};
}

Session::Reply Session::on_context_stop() {
    leave_room_quietly();
    {
        std::lock_guard<std::mutex> lk(pump_mu_);
        pump_stop_ = true;
    }
    pump_cv_.notify_all();
    return {sn::err::NoError, ""};
}

Session::Reply Session::on_get_world_info_list() {
    const std::string world = pb::Writer().u(1, 1).u(5, 1).str();
    return {sn::err::NoError, sn::proto_blob(pb::Writer().msg(1, world).str())};
}

json Session::host_extra_from_advertisement() const {
    // bbhost's create_room extras (HostArea/HostLevel/HostPos/MemberTag),
    // approximated from the sign the port uploaded through our WebAPI.
    const json adv = advertisement_.is_object() ? advertisement_ : bridge_.last_advertisement();
    if (!adv.is_object()) return json();
    json extra = json::object();
    if (jhas(adv, "AreaId")) extra["HostArea"] = jint(adv, "AreaId");
    if (jhas(adv, "MatchingLevel")) extra["HostLevel"] = jint(adv, "MatchingLevel");
    extra["HostPos"] = {0.0, 0.0, 0.0};
    extra["MemberTag"] = 1;
    return extra;
}

Session::Reply Session::on_create_room(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    int max_slots = static_cast<int>(pb::Message(blob).u(2, 5));
    if (!max_slots) max_slots = 5;
    const auto [local, mapped] = p2p_addresses();
    json reply;
    try {
        reply = bb.create_room(bb.online_id().empty() ? npid_ : bb.online_id(), local, mapped, max_slots,
                               host_extra_from_advertisement());
    } catch (const BridgeError& e) {
        log(std::string("create_room failed: ") + e.what());
        return {sn::err::Invalid, ""};
    }
    room_.clear();
    forget_peers();
    room_.room_id = static_cast<uint64_t>(jint(reply, "RoomId", 0));
    room_.session_id = jstr(reply, "SessionId");
    room_.member_id = static_cast<int>(jint(reply, "MemberId", 1));
    room_.owner_id = static_cast<int>(jint(reply, "OwnerMemberId", room_.member_id));
    room_.max_slot = static_cast<int>(jint(reply, "MaxMembers", max_slots));
    absorb_members(reply.value("Members", json()));
    ensure_self_member();
    const std::string details = build_details(room_.member_id, room_.owner_id);
    pb::Writer w;
    w.u(1, room_.room_id).u(2, 1).u(3, 1).u(4, 1).u(5, static_cast<uint64_t>(room_.member_id))
        .u(6, static_cast<uint64_t>(room_.max_slot)).msg(9, details);
    log(fmt("room %llu created (member %d, max %d)", static_cast<unsigned long long>(room_.room_id), room_.member_id,
            room_.max_slot));
    return {sn::err::NoError, sn::proto_blob(w.str())};
}

Session::Reply Session::on_join_room(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    const uint64_t room_id = pb::Message(blob).u(1);
    if (!room_id) return {sn::err::InvalidInput, ""};
    const auto [local, mapped] = p2p_addresses();
    json reply;
    try {
        reply = bb.join_room(bb.online_id().empty() ? npid_ : bb.online_id(), room_id, local, mapped);
    } catch (const BridgeError& e) {
        log(fmt("join_room(%llu) failed: %s", static_cast<unsigned long long>(room_id), e.what()));
        return {sn::err::RoomMissing, ""};
    }
    room_.clear();
    // keep the host a guest_invite seeded for this room: the port may already know it there
    if (bridge_.gateway) {
        std::set<std::string> invited;
        for (const auto& e : pending_guest_invites_)
            if (static_cast<uint64_t>(jint(e, "RoomId")) == room_id) invited.insert(jstr(e, "HostOnlineId"));
        bridge_.gateway->remove_peers_except(invited);
    }
    room_.room_id = room_id;
    room_.session_id = jstr(reply, "SessionId");
    room_.member_id = static_cast<int>(jint(reply, "MemberId", 2));
    room_.owner_id = static_cast<int>(jint(reply, "OwnerMemberId", 1));
    room_.max_slot = static_cast<int>(jint(reply, "MaxMembers", 5));
    absorb_members(reply.value("Members", json()));
    ensure_self_member();
    const std::string details = build_details(room_.member_id, room_.owner_id);
    pb::Writer w;
    w.u(1, room_id).u(2, static_cast<uint64_t>(room_.member_id)).u(3, static_cast<uint64_t>(room_.max_slot)).msg(6, details);
    log(fmt("room %llu joined (member %d of max %d)", static_cast<unsigned long long>(room_id), room_.member_id,
            room_.max_slot));
    return {sn::err::NoError, sn::proto_blob(w.str())};
}

Session::Reply Session::on_leave_room(const std::string& payload) {
    leave_room_quietly();
    std::string blob;
    const uint64_t room_id = sn::parse_proto_blob(payload, blob) ? pb::Message(blob).u(1) : 0;
    return {sn::err::NoError, sn::proto_blob(pb::Writer().u(1, room_id).str())};
}

Session::Reply Session::on_request_signaling_infos(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    const std::string target = pb::Message(blob).s(1);
    auto reply = [&](const Endpoint& e) -> Reply {
        return {sn::err::NoError, sn::proto_blob(pb::Writer().s(1, target).s(2, e.ip).u(3, e.port).str())};
    };
    const json* member = nullptr;
    for (const auto& [mid, m] : room_.members)
        if (jstr(m, "OnlineId") == target) member = &m;
    if (bridge_.gateway) {
        // the port must talk to the member's inner socket, never directly
        if (auto inner = bridge_.gateway->inner_endpoint_for(target)) return reply(*inner);
        if (member) return reply(port_endpoint_for(*member));
        // Not in our room yet: a host's game signals the guest it summons before
        // the guest joins (the summon goes over P2P). Ask the server where that
        // player is, as shadNet does.
        const std::optional<Endpoint> resolved = bb.resolve_peer(target);
        if (!resolved) {
            log(fmt("signaling: %s is not registered on the server", repr(target).c_str()));
            return {sn::err::NotFound, ""};
        }
        const int key = -static_cast<int>(std::hash<std::string>{}(target) % 30000 + 1);   // provisional
        const Endpoint inner = bridge_.gateway->add_peer(key, target, *resolved, Endpoint{}, npid_, room_.member_id,
                                                         room_.room_id, port_p2p_endpoint());
        log(fmt("signaling: %s resolved to %s (not in the room yet)", repr(target).c_str(), resolved->str().c_str()));
        return reply(inner);
    }
    std::optional<Endpoint> resolved = bb.resolve_peer(target);
    if (!resolved && member) resolved = member_endpoint(*member);
    if (!resolved || !resolved->valid()) return {sn::err::NotFound, ""};
    return reply(*resolved);
}

Session::Reply Session::on_room_id_echo(const std::string& payload) {
    std::string blob;
    const uint64_t room_id = sn::parse_proto_blob(payload, blob) ? pb::Message(blob).u(2) : 0;
    return {sn::err::NoError, sn::proto_blob(pb::Writer().u(1, room_id).str())};
}

Session::Reply Session::on_kickout(const std::string& payload) {
    std::string blob;
    if (!sn::parse_proto_blob(payload, blob)) return {sn::err::Malformed, ""};
    pb::Message req(blob);
    try {
        bb.kick_member(room_.session_id, static_cast<int>(req.u(3)), room_.member_id);
    } catch (const BridgeError& e) {
        log(std::string("kick failed: ") + e.what());
        return {sn::err::Invalid, ""};
    }
    return {sn::err::NoError, sn::proto_blob(pb::Writer().u(1, req.u(1)).str())};
}

Endpoint Session::member_endpoint(const json& member) {
    // bbhost's own rule: a STUN-mapped address differing from the local one
    // means NAT; only the mapped one reaches the peer.
    const std::string mapped = jstr(member, "MappedAddr"), local = jstr(member, "LocalAddr"), addr = jstr(member, "Addr");
    const int mapped_port = static_cast<int>(jint(member, "MappedPort")), port = static_cast<int>(jint(member, "Port"));
    if (!mapped.empty() && mapped_port > 0 && mapped_port <= 65535 && mapped != local && mapped != addr)
        return {mapped, mapped_port};
    if (!addr.empty() && port) return {addr, port};
    return {local, static_cast<int>(jint(member, "LocalPort"))};
}

std::optional<Endpoint> Session::port_p2p_endpoint() const {
    // The port's P2P UDP socket: from its STUN ping when one came, else the
    // pinned port. The gateway also learns it from the port's first datagram;
    // a host that speaks first (guest invite) needs it before that.
    if (auto e = bridge_.registry_get(npid_)) return e;
    if (bridge_.cfg.port_p2p_port > 0) return Endpoint{"127.0.0.1", bridge_.cfg.port_p2p_port};
    return std::nullopt;
}

Endpoint Session::port_endpoint_for(const json& member) {
    // Where the port should send to reach this member: the gateway's inner
    // socket for a remote member, else bbhost's endpoint rule.
    const int mid = static_cast<int>(jint(member, "MemberId"));
    if (bridge_.gateway && mid && mid != room_.member_id) {
        const Endpoint local{jstr(member, "LocalAddr"), static_cast<int>(jint(member, "LocalPort"))};
        return bridge_.gateway->add_peer(mid, jstr(member, "OnlineId"), member_endpoint(member), local, npid_,
                                         room_.member_id, room_.room_id, port_p2p_endpoint());
    }
    return member_endpoint(member);
}

void Session::forget_peers(std::optional<int> member_id) {
    if (!bridge_.gateway) return;
    if (member_id) bridge_.gateway->remove_peer(*member_id);
    else bridge_.gateway->clear_peers();
}

void Session::ensure_self_member() {
    // bbport fills the game's members.me / members.owner pointers only from the
    // member list (np_matching2_internal.cpp:265). The server's create_room
    // reply lists no members, so the game got null pointers and crashed; bbhost
    // builds the response with its own member in it, and so does the bridge.
    const int mid = room_.member_id;
    if (mid && !room_.members.count(mid)) {
        const auto [local, mapped] = p2p_addresses();
        room_.members[mid] = {{"MemberId", mid}, {"OnlineId", npid_},     {"Addr", mapped.ip},
                              {"Port", mapped.port}, {"LocalAddr", local.ip}, {"LocalPort", local.port}};
    }
}

void Session::absorb_members(const json& members) {
    if (!members.is_array()) return;
    for (const auto& m : members) {
        const int mid = static_cast<int>(jint(m, "MemberId"));
        if (mid) room_.members[mid] = m;
    }
}

std::string Session::member_data(const json& member, int owner_id) {
    const int mid = static_cast<int>(jint(member, "MemberId"));
    const Endpoint e = port_endpoint_for(member);
    pb::Writer w;
    w.s(1, jstr(member, "OnlineId")).u(2, static_cast<uint64_t>(mid)).flag(4, mid == owner_id).u(8, 1)
        .u(9, mid == owner_id ? sn::kOwnerFlag : 0).s(10, e.ip).u(11, static_cast<uint64_t>(e.port));
    return w.str();
}

std::string Session::build_details(int me_id, int owner_id) {
    std::vector<std::string> members;
    uint64_t joined = 0;
    for (const auto& [mid, m] : room_.members) {
        members.push_back(member_data(m, owner_id));
        if (mid > 0 && mid <= 64) joined |= 1ull << (mid - 1);
    }
    const int open = std::max(room_.max_slot - static_cast<int>(members.size()), 0);
    pb::Writer room;
    room.u(1, static_cast<uint64_t>(room_.max_slot)).u(3, static_cast<uint64_t>(open))
        .u(5, static_cast<uint64_t>(room_.max_slot)).u(6, 1).u(7, 1).u(8, 1).u(9, room_.room_id).u(11, joined);
    pb::Writer w;
    w.msg(1, room.str());
    for (const auto& m : members) w.msg(2, m);
    w.u(3, static_cast<uint64_t>(me_id)).u(4, static_cast<uint64_t>(owner_id));
    return w.str();
}

void Session::leave_room_quietly() {
    if (!room_.room_id) return;
    try {
        bb.leave_room(room_.session_id, room_.member_id);
        log(fmt("left room %llu", static_cast<unsigned long long>(room_.room_id)));
    } catch (const BridgeError& e) {
        log(std::string("leave_room failed: ") + e.what());
    }
    room_.clear();
    forget_peers();
}

void Session::send_room_event(uint32_t event, uint32_t cause, const json& member, int64_t error_code) {
    pb::Writer w;
    w.u(1, ctx_id_).u(2, room_.room_id).u(3, event).u(4, cause)
        .u(5, error_code < 0 ? (static_cast<uint64_t>(error_code) & 0xFFFFFFFFull) : static_cast<uint64_t>(error_code));
    if (jint(member, "MemberId")) w.msg(6, member_data(member, room_.owner_id));
    conn->send(sn::build_notification(sn::notify::RoomEvent, w.str()));
}

void Session::start_pump() {
    {
        std::lock_guard<std::mutex> lk(pump_mu_);
        pump_stop_ = false;
    }
    std::lock_guard<std::mutex> lk(join_mu_);
    if (pump_running_) return;   // still running: the fresh flag lets it go on
    if (pump_.joinable()) pump_.join();   // ended after a ContextStop
    pump_running_ = true;
    pump_ = std::thread([this] {
        pump_main();
        pump_running_ = false;
    });
}

void Session::pump_main() {
    uint64_t cursor = 0;
    double last_beat = -1e9;
    int poll_failures = 0;
    auto wait = [this](double seconds) {
        std::unique_lock<std::mutex> lk(pump_mu_);
        pump_cv_.wait_for(lk, std::chrono::milliseconds(static_cast<int>(seconds * 1000)), [this] { return pump_stop_; });
        return !pump_stop_;
    };
    while (true) {
        {
            std::lock_guard<std::mutex> lk(pump_mu_);
            if (pump_stop_) break;
        }
        if (!conn->alive || !logged_in_) break;
        std::string session_id, online;
        int member = 0;
        uint64_t room = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            session_id = room_.session_id;
            member = room_.member_id;
            room = room_.room_id;
            online = bb.online_id().empty() ? npid_ : bb.online_id();
        }
        if (room && now() - last_beat >= kHeartbeatInterval) {
            last_beat = now();
            if (bb.heartbeat(session_id, member) == 0) {
                std::lock_guard<std::recursive_mutex> lk(mu_);
                log(fmt("server says we are no longer in room %llu", static_cast<unsigned long long>(room_.room_id)));
                room_.clear();
                forget_peers();
            }
        }
        try {
            json event;
            cursor = bb.poll_events(online, cursor, event);
            poll_failures = 0;
            if (!event.is_null()) {
                {
                    std::lock_guard<std::recursive_mutex> lk(mu_);
                    handle_event(event);
                }
                bb.ack_event(online, cursor);
            }
        } catch (const BridgeError& e) {
            // One miss is retried at once (an invite should not wait); a run of
            // them is logged and slowed down.
            if (++poll_failures >= 2) log(fmt("event poll failed (%d in a row): %s", poll_failures, e.what()));
            if (poll_failures >= 2 && !wait(3.0)) break;
            continue;
        }
        if (!wait(bridge_.cfg.poll_interval)) break;
    }
}

void Session::handle_event(const json& event) {
    const std::string name = jstr(event, "Name");
    if (name == "room_member_joined") {
        const int mid = static_cast<int>(jint(event, "MemberId"));
        absorb_members(json::array({event}));
        log(fmt("event: member %d (%s) joined room %s", mid, jstr(event, "OnlineId", "?").c_str(),
                jstr(event, "RoomId").c_str()));
        send_room_event(sn::room_event::MemberJoined, sn::cause::ServerOperation, event);
    } else if (name == "room_member_left") {
        const int mid = static_cast<int>(jint(event, "MemberId"));
        const std::string reason = jstr(event, "Reason");
        const uint32_t c = reason == "kicked" ? sn::cause::Kickout
                           : (reason == "leave_room" || reason.empty()) ? sn::cause::Leave
                                                                         : sn::cause::MemberDisappeared;
        log(fmt("event: member %d left (%s)", mid, reason.empty() ? "?" : reason.c_str()));
        send_room_event(sn::room_event::MemberLeft, c, event);
        room_.members.erase(mid);
        forget_peers(mid);
    } else if (name == "room_closed") {
        log(fmt("event: room %s closed (%s)", jstr(event, "RoomId").c_str(), jstr(event, "Reason", "?").c_str()));
        for (const auto& [mid, m] : room_.members) send_room_event(sn::room_event::MemberLeft, sn::cause::Leave, m);
        room_.clear();
        forget_peers();
    } else if (name == "room_member_kicked") {
        log(fmt("event: we were kicked from room %s", jstr(event, "RoomId").c_str()));
        send_room_event(sn::room_event::KickedOut, sn::cause::Kickout, event, -1);
        room_.clear();
        forget_peers();
    } else if (name == "guest_invite") {
        // A host summoned our sign. As bbhost does (np_matching2.cpp
        // on_server_event): seed the host as member 1 now, so its game's first
        // P2P datagrams reach the port through an inner socket; the port's game
        // then joins, and JoinRoom's answer confirms the same member.
        const uint64_t room_id = static_cast<uint64_t>(jint(event, "RoomId"));
        const json host = {{"MemberId", 1},
                           {"OnlineId", jstr(event, "HostOnlineId")},
                           {"Addr", jstr(event, "HostAddr")},
                           {"Port", jint(event, "HostPort")},
                           {"LocalAddr", jstr(event, "HostLocalAddr")},
                           {"LocalPort", jint(event, "HostLocalPort")},
                           {"MappedAddr", jstr(event, "HostMappedAddr")},
                           {"MappedPort", jint(event, "HostMappedPort")}};
        log(fmt("event: guest_invite for room %llu from %s at %s:%lld (area %s level %s)",
                static_cast<unsigned long long>(room_id), jstr(host, "OnlineId").c_str(), jstr(host, "Addr").c_str(),
                static_cast<long long>(jint(host, "Port")), jstr(event, "HostArea").c_str(),
                jstr(event, "HostLevel").c_str()));
        pending_guest_invites_.push_back(event);
        if (bridge_.gateway && !jstr(host, "OnlineId").empty()) {
            const Endpoint local{jstr(host, "LocalAddr"), static_cast<int>(jint(host, "LocalPort"))};
            bridge_.gateway->add_peer(1, jstr(host, "OnlineId"), member_endpoint(host), local, npid_, 0, room_id,
                                      port_p2p_endpoint());
        }
    } else {
        log(fmt("event: %s (untranslated)", name.c_str()));
    }
}

void Session::shutdown() {
    {
        std::lock_guard<std::mutex> lk(pump_mu_);
        pump_stop_ = true;
    }
    pump_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(join_mu_);
        if (pump_.joinable() && pump_.get_id() != std::this_thread::get_id()) pump_.join();
    }
    std::lock_guard<std::recursive_mutex> lk(mu_);
    leave_room_quietly();
}

// ---- bridge ----

json Bridge::load_state() {
    std::lock_guard<std::mutex> lk(state_mu_);
    std::ifstream in(widen(cfg.state_file));
    if (!in) return json::object();
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        log("state file is corrupt; starting fresh");
        return json::object();
    }
    return j;
}

void Bridge::save_state(const json& state) {
    std::lock_guard<std::mutex> lk(state_mu_);
    const std::wstring path = widen(cfg.state_file);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << state.dump(2);
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);   // never a half-written account file
}

std::optional<Endpoint> Bridge::registry_get(const std::string& npid) const {
    std::lock_guard<std::mutex> lk(registry_mu_);
    auto it = registry_.find(npid);
    if (it == registry_.end()) return std::nullopt;
    return it->second;
}

std::pair<std::string, std::string> Bridge::identity() const {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    for (const auto& s : sessions_)
        if (s->logged_in() && !s->bb.token().empty()) return {s->bb.token(), s->bb.ruleset};
    return {"", ""};
}

json Bridge::last_advertisement() const {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    return last_advertisement_;
}

void Bridge::on_mapped_change(const Endpoint& mapped) {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        sessions = sessions_;
    }
    for (auto& s : sessions) s->on_mapped_change(mapped);
}

bool Bridge::start(std::string* error) {
    net_init();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        log(why);
        stop();
        return false;
    };
    running_ = true;
    if (cfg.p2p_gateway) {
        const std::string lan = cfg.local_addr.empty() ? detect_lan_addr() : cfg.local_addr;
        gateway = std::make_unique<p2p::Gateway>(cfg.p2p_port, cfg.stun_endpoint(), lan,
                                                 [this](const Endpoint& m) { on_mapped_change(m); });
        gateway->stun_refresh = cfg.stun_refresh;
        std::string why;
        if (!gateway->start(&why)) return fail(why);
    }
    tcp_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(tcp_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = to_sockaddr(Endpoint{cfg.bind_host, cfg.tcp_port});
    if (bind(tcp_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(tcp_, 8) != 0)
        return fail(fmt("cannot listen on tcp %s:%d - is another bridge running? (error %d)", cfg.bind_host.c_str(),
                        cfg.tcp_port, WSAGetLastError()));
    stun_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in uaddr = to_sockaddr(Endpoint{cfg.bind_host, cfg.udp_port});
    if (bind(stun_, reinterpret_cast<sockaddr*>(&uaddr), sizeof(uaddr)) != 0)
        return fail(fmt("cannot bind udp %s:%d (error %d)", cfg.bind_host.c_str(), cfg.udp_port, WSAGetLastError()));
    std::string why;
    if (!webapi_server_.start(cfg.bind_host, cfg.webapi_port, [this](const http::ServerRequest& r) { return webapi(r); },
                              &why))
        return fail(why);
    tcp_thread_ = std::thread([this] { tcp_accept_loop(); });
    stun_thread_ = std::thread([this] { stun_loop(); });
    log(fmt("listening: tcp :%d  stun :%d  webapi :%d", cfg.tcp_port, cfg.udp_port, webapi_server_.port()));
    for (const auto& [port, upstream] : cfg.game_proxy_routes) {
        auto server = std::make_unique<http::Server>();
        const std::string up = upstream;
        if (!server->start(cfg.bind_host, port, [this, up](const http::ServerRequest& r) { return game_proxy(up, r); },
                           &why))
            return fail(why);
        log(fmt("game proxy :%d -> %s", server->port(), upstream.c_str()));
        proxies_.push_back(std::move(server));
    }
    log("bbhost server: " + cfg.bbhost_server);
    log("community server: " + cfg.community_server);
    return true;
}

void Bridge::stop() {
    if (!running_.exchange(false)) return;
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        sessions = sessions_;
    }
    for (auto& s : sessions) {
        s->shutdown();
        s->conn->close();
    }
    if (gateway) gateway->stop();
    for (auto& p : proxies_) p->stop();
    proxies_.clear();
    webapi_server_.stop();
    if (tcp_ != INVALID_SOCKET) closesocket(tcp_);
    if (stun_ != INVALID_SOCKET) closesocket(stun_);
    tcp_ = stun_ = INVALID_SOCKET;
    if (tcp_thread_.joinable()) tcp_thread_.join();
    if (stun_thread_.joinable()) stun_thread_.join();
    for (int i = 0; i < 500 && tcp_active_.load() > 0; ++i) Sleep(10);
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        sessions_.clear();
    }
    gateway.reset();
}

BridgeStatus Bridge::status() const {
    BridgeStatus st;
    st.running = running_;
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        if (!sessions_.empty()) {
            const auto& s = sessions_.front();
            st.npid = s->npid();
            st.logged_in = s->logged_in();
            st.room_id = s->room_id();
            st.room_members = s->room_members();
        }
    }
    if (gateway) {
        st.relay_port = gateway->relay_port();
        st.mapped = gateway->mapped();
        st.peers = gateway->peers();
    }
    return st;
}

void Bridge::tcp_accept_loop() {
    while (running_) {
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = accept(tcp_, reinterpret_cast<sockaddr*>(&from), &len);
        if (s == INVALID_SOCKET) break;
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        log("tcp: connection from " + from_sockaddr(from).str());
        ++tcp_active_;
        std::thread([this, s] {
            tcp_serve(std::make_shared<TcpConn>(s));
            --tcp_active_;
        }).detach();
    }
}

void Bridge::tcp_serve(std::shared_ptr<TcpConn> conn) {
    std::shared_ptr<Session> session;
    if (conn->send(sn::build_server_info())) {
        std::string header(sn::kHeaderSize, '\0');
        while (running_ && conn->alive) {
            if (!read_exact(conn->sock, header.data(), header.size())) break;
            sn::Header h;
            sn::parse_header(header, h);
            if (h.size < sn::kHeaderSize || h.size > sn::kMaxPacketSize) break;
            std::string payload(h.size - sn::kHeaderSize, '\0');
            if (!payload.empty() && !read_exact(conn->sock, payload.data(), payload.size())) break;
            if (h.type != sn::Request) continue;
            if (!session) {
                session = std::make_shared<Session>(*this, conn);
                std::lock_guard<std::mutex> lk(sessions_mu_);
                sessions_.push_back(session);
            }
            const auto [error, reply] = session->handle(h.command, h.packet_id, payload);
            if (!conn->send(sn::build_reply(h.command, h.packet_id, error, reply))) break;
            if (session->terminate_requested) break;   // shadNet closes after Terminate's reply
        }
    }
    conn->alive = false;
    closesocket(conn->sock);
    if (session) {
        session->shutdown();
        {
            std::lock_guard<std::mutex> lk(sessions_mu_);
            sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
        }
        const std::string npid = session->npid();
        log(fmt("tcp: session for %s closed", npid.empty() ? "?" : repr(npid).c_str()));
    }
}

void Bridge::stun_loop() {
    // shadNet's endpoint discovery: cmd 0x01 + npid[16] + local_ip[4] -> the
    // sender's (ip, port) in network order.
    char buf[4096];
    while (running_) {
        sockaddr_in from{};
        int len = sizeof(from);
        const int n = recvfrom(stun_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) {
            if (!running_) return;
            continue;
        }
        if (n < 21 || buf[0] != 0x01) continue;
        const std::string npid(buf + 1, strnlen(buf + 1, 16));
        const Endpoint src = from_sockaddr(from);
        bool first;
        {
            std::lock_guard<std::mutex> lk(registry_mu_);
            auto it = registry_.find(npid);
            first = it == registry_.end() || it->second != src;
            registry_[npid] = src;
        }
        if (first) log(fmt("stun: %s pings from %s", repr(npid).c_str(), src.str().c_str()));
        char reply[6];
        std::memcpy(reply, &from.sin_addr, 4);
        std::memcpy(reply + 4, &from.sin_port, 2);
        sendto(stun_, reply, 6, 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
}

http::ServerResponse Bridge::webapi(const http::ServerRequest& req) {
    const std::string path = req.path();
    std::vector<std::string> parts;
    for (size_t a = 1; a <= path.size();) {
        const size_t b = path.find('/', a);
        parts.push_back(path.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (req.method == "GET" && path == "/status")
        return http::ServerResponse::json({{"ok", true}, {"bridge", "bbbridge"}, {"version", kVersion}});
    if (req.method == "GET" && path == "/debug/state" && cfg.debug_endpoints) {
        json registry = json::object();
        {
            std::lock_guard<std::mutex> lk(registry_mu_);
            for (const auto& [npid, e] : registry_) registry[npid] = {e.ip, e.port};
        }
        json peers = json::array();
        if (gateway)
            for (const auto& p : gateway->peers())
                peers.push_back({{"member_id", p.member_id}, {"online_id", p.online_id},
                                 {"inner", {p.inner.ip, p.inner.port}}, {"peer_member_id", p.peer_member_id},
                                 {"port_member_id", p.port_member_id}, {"room_id", p.room_id}});
        json mapped = json();
        json relay = 0;
        if (gateway) {
            if (auto m = gateway->mapped()) mapped = {m->ip, m->port};
            relay = gateway->relay_port();
        }
        return http::ServerResponse::json({{"registry", registry},
                                           {"last_advertisement", last_advertisement()},
                                           {"gateway", gateway != nullptr},
                                           {"mapped", mapped},
                                           {"relay_port", relay},
                                           {"peers", peers}});
    }
    if (req.method == "POST" && path == "/debug/add_peer" && cfg.debug_endpoints && gateway) {
        // tests drive the gateway directly, as the Python suite does with gw.add_peer
        const json b = json::parse(req.body, nullptr, false);
        auto ep = [&](const char* key) {
            const json e = b.is_object() && b.contains(key) ? b[key] : json();
            return e.is_array() && e.size() == 2 ? Endpoint{e[0].get<std::string>(), e[1].get<int>()} : Endpoint{};
        };
        const Endpoint port_ep = ep("port_endpoint");
        const Endpoint inner = gateway->add_peer(
            static_cast<int>(jint(b, "member_id")), jstr(b, "online_id"), ep("endpoint"), ep("local_endpoint"),
            jstr(b, "port_online_id"), static_cast<int>(jint(b, "port_member_id")),
            static_cast<uint64_t>(jint(b, "room_id")),
            port_ep.valid() ? std::optional<Endpoint>(port_ep) : std::nullopt);
        return http::ServerResponse::json({{"inner", {inner.ip, inner.port}}});
    }
    if (req.method == "POST" && path == "/debug/remove_peer" && cfg.debug_endpoints && gateway) {
        const json b = json::parse(req.body, nullptr, false);
        gateway->remove_peer(static_cast<int>(jint(b, "member_id")));
        return http::ServerResponse::json(json::object());
    }
    // The game's userProfile calls (libSceNpWebApi through the port). shadNet
    // answers from its friend table; the bridge keeps none, so the lists are
    // empty - but they must be 200: Bloodborne fetches them at sign-in and a
    // 404 left it short of a working online state.
    if (parts.size() == 4 && parts[0] == "v1" && parts[1] == "users") {
        if (req.method == "GET" && (parts[3] == "friendList" || parts[3] == "blockList" || parts[3] == "blockingUsers")) {
            int offset = 0;
            const std::string q = req.query();
            const size_t at = q.find("offset=");
            if (at != std::string::npos && (at == 0 || q[at - 1] == '&')) offset = std::atoi(q.c_str() + at + 7);
            return http::ServerResponse::json(
                {{parts[3], json::array()}, {"start", offset}, {"size", 0}, {"totalResults", 0}});
        }
        if (req.method == "POST" && parts[3] == "feed")   // activity-feed stories: nothing to keep them in
            return http::ServerResponse::json(json::object(), 201);
    }
    if (req.method == "POST" && path.rfind("/summon_messenger/", 0) == 0) return summon(req);
    log(fmt("webapi: %s %s not handled (404)", req.method.c_str(), path.c_str()));
    return http::ServerResponse::json({{"ResKind", 1}, {"Message", "not found"}}, 404);
}

http::ServerResponse Bridge::summon(const http::ServerRequest& req) {
    // The game's sign API, to the community server (the shared world).
    const std::string path = req.path();
    if (path == "/summon_messenger/create") {
        json parsed = req.body.empty() ? json() : json::parse(req.body, nullptr, false);
        if (parsed.is_discarded()) parsed = json();
        if (parsed.is_object() && jhas(parsed, "SessionId")) {
            std::vector<std::shared_ptr<Session>> sessions;
            {
                std::lock_guard<std::mutex> lk(sessions_mu_);
                last_advertisement_ = parsed;
                sessions = sessions_;
            }
            for (auto& s : sessions) s->set_advertisement(parsed);
        }
        if (parsed.is_null() || parsed.empty())   // the launcher's probe posts an empty body
            return http::ServerResponse::json({{"ResKind", 1}, {"Message", "empty summon advertisement"}}, 400);
    }
    http::ClientRequest up;
    up.method = "POST";
    up.url = cfg.community_server + path;
    up.body = req.body;
    up.headers = {{"Content-Type", "application/json"}, {"User-Agent", cfg.user_agent}};
    // bbhost sends the account token on the game's own traffic too, so the
    // server maps the game session - and its signs - to the room's account.
    const auto [token, ruleset] = identity();
    if (!token.empty()) up.headers.emplace_back("X-BB-Token", token);
    if (!ruleset.empty()) up.headers.emplace_back("X-BBHost-Ruleset", ruleset);
    const http::ClientResponse res = http::send(up);
    if (!res.ok()) {
        log(fmt("summon %s: upstream unreachable: %s", path.c_str(), res.error.c_str()));
        return http::ServerResponse::json({{"ResKind", 1}, {"Message", "upstream unreachable: " + res.error}}, 502);
    }
    const std::string placement = http::header_value(res.headers, "X-ShadPS4-Bloodborne-Host-Placement");
    log(fmt("summon %s: %zu bytes -> HTTP %d, %zu bytes%s", path.c_str(), req.body.size(), res.status, res.body.size(),
            placement.empty() ? "" : (", placement " + placement).c_str()));
    if (cfg.trace)
        capture(path.substr(path.rfind('/') + 1), req.body, res.status, res.body, "placement=" + placement);
    http::ServerResponse out;
    out.status = res.status;
    out.body = res.body;
    out.headers.emplace_back("Content-Type", "application/json");
    if (!placement.empty()) out.headers.emplace_back("X-ShadPS4-Bloodborne-Host-Placement", placement);
    return out;
}

http::ServerResponse Bridge::game_proxy(const std::string& upstream, const http::ServerRequest& req) {
    // The game's own server traffic: the account token added as bbhost does,
    // so the server's game sessions map to the account.
    http::ClientRequest up;
    up.method = req.method;
    up.url = upstream + req.target;
    up.body = req.body;
    up.timeout_ms = 15000;
    std::string ua;
    for (const auto& [k, v] : req.headers) {
        const std::string lk = lower(k);
        if (kHopByHop.count(lk)) continue;
        if (lk == "user-agent") { ua = v; continue; }
        up.headers.emplace_back(k, v);
    }
    up.headers.emplace_back("User-Agent", ua.empty() ? cfg.user_agent : ua + " " + cfg.user_agent);
    const auto [token, ruleset] = identity();
    if (!token.empty()) up.headers.emplace_back("X-BB-Token", token);
    if (!ruleset.empty()) up.headers.emplace_back("X-BBHost-Ruleset", ruleset);
    const http::ClientResponse res = http::send(up);
    const std::string path = req.path();
    if (!res.ok()) {
        log(fmt("game %s %s: upstream unreachable: %s", req.method.c_str(), path.c_str(), res.error.c_str()));
        http::ServerResponse r;
        r.status = 502;
        return r;
    }
    if (cfg.trace) {
        const std::string port = upstream.substr(upstream.rfind(':') + 1);
        log(fmt("game %s %s %s: %zu bytes -> HTTP %d, %zu bytes", req.method.c_str(), port.c_str(), path.c_str(),
                req.body.size(), res.status, res.body.size()));
        for (const char* p : {"/basic_utils/login", "/blood_messenger/", "/tomb_messenger/", "/wandering_ghost/get"})
            if (path.rfind(p, 0) == 0) {
                std::string name = path.substr(1);
                std::replace(name.begin(), name.end(), '/', '_');
                capture(name, req.body, res.status, res.body, "");
                break;
            }
    }
    http::ServerResponse out;
    out.status = res.status;
    out.body = res.body;
    for (const auto& [k, v] : res.headers)
        if (!kHopByHop.count(lower(k))) out.headers.emplace_back(k, v);
    return out;
}

void Bridge::capture(const std::string& name, const std::string& request, int status, const std::string& response,
                     const std::string& note) {
    // Trace mode keeps each exchange under captures/ next to the state file.
    try {
        const fs::path dir = fs::absolute(fs::path(widen(cfg.state_file))).parent_path() / L"captures";
        fs::create_directories(dir);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        const std::wstring stem = (dir / widen(std::string(stamp) + "-" + name)).wstring();
        std::ofstream(stem + L"-request.bin", std::ios::binary) << request;
        std::ofstream(stem + L"-response.bin", std::ios::binary) << response;
        std::ofstream(stem + L".txt") << "name=" << name << "\nstatus=" << status << "\n" << note << "\n";
    } catch (const std::exception& e) {
        log(std::string("capture failed: ") + e.what());
    }
}

}  // namespace bb
