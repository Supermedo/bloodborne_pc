#include "bbhost_client.hpp"

#include "http.hpp"

namespace bb {

BBHostClient::Reply BBHostClient::post(const std::string& path, const json& body, int timeout_ms, http::Pool* pool) {
    http::ClientRequest req;
    req.method = "POST";
    req.url = base_ + path;
    req.body = body.dump();
    req.timeout_ms = timeout_ms;
    req.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}, {"User-Agent", user_agent}};
    const std::string tok = token();
    if (!tok.empty()) req.headers.emplace_back("X-BB-Token", tok);
    if (!ruleset.empty()) req.headers.emplace_back("X-BBHost-Ruleset", ruleset);
    http::ClientResponse res = pool ? pool->send(req) : http::send(req);
    if (!res.ok()) throw BridgeError(path + ": " + res.error);
    Reply r;
    r.status = res.status;
    r.body = json::parse(res.body, nullptr, false);
    if (r.body.is_discarded()) r.body = json::object();
    return r;
}

json BBHostClient::call(const std::string& path, const json& body, int timeout_ms) {
    Reply r = post(path, body, timeout_ms);
    if (r.status < 200 || r.status >= 300)
        throw BridgeError(fmt("%s: HTTP %d %s", path.c_str(), r.status, jstr(r.body, "Message").c_str()));
    if (!r.body.is_object() || !r.body.contains("ResKind"))
        throw BridgeError(fmt("%s: no account/matching service (HTTP %d)", path.c_str(), r.status));
    if (jint(r.body, "ResKind", -1) != 0)
        throw BridgeError(path + ": refused: " + jstr(r.body, "Message", "unknown"));
    return r.body;
}

std::string BBHostClient::account_create(const std::string& name) {
    json r = call("/auth/account/create", {{"Name", name}, {"DeviceName", "bbbridge"}});
    set_identity(jstr(r, "Token"), jstr(r, "OnlineId", name));
    return jstr(r, "RecoveryCode");
}

std::string BBHostClient::account_recover(const std::string& name, const std::string& code) {
    json r = call("/auth/recover", {{"Name", name}, {"RecoveryCode", code}});
    set_identity(jstr(r, "Token"), jstr(r, "OnlineId", name));
    return jstr(r, "RecoveryCode");
}

std::optional<bool> BBHostClient::token_valid(const std::string& online_id) {
    try {
        Reply r = post("/np/signaling/resolve", {{"OnlineId", online_id}}, 6000);
        if (r.status == 401) return false;
        if (r.status >= 200 && r.status < 500) return true;
    } catch (const BridgeError&) {
    }
    return std::nullopt;
}

json BBHostClient::context_start(const std::string& online_id, const Endpoint& signaling, const Endpoint& mapped) {
    return call("/mp/matching2/context_start", {{"OnlineId", online_id},
                                                {"SignalingAddr", signaling.ip},
                                                {"SignalingPort", signaling.port},
                                                {"MappedAddr", mapped.ip},
                                                {"MappedPort", mapped.port}});
}

json BBHostClient::create_room(const std::string& online_id, const Endpoint& local, const Endpoint& mapped,
                               int max_members, const json& extra) {
    json body = {{"OnlineId", online_id}, {"LocalAddr", local.ip},   {"LocalPort", local.port},
                 {"PublicAddr", local.ip}, {"PublicPort", local.port}, {"MappedAddr", mapped.ip},
                 {"MappedPort", mapped.port}, {"MaxMembers", max_members}};
    if (extra.is_object())
        for (auto it = extra.begin(); it != extra.end(); ++it) body[it.key()] = it.value();
    return call("/mp/matching2/create_room", body);
}

json BBHostClient::join_room(const std::string& online_id, uint64_t room_id, const Endpoint& local,
                             const Endpoint& mapped) {
    return call("/mp/matching2/join_room", {{"OnlineId", online_id}, {"RoomId", room_id},
                                            {"LocalAddr", local.ip}, {"LocalPort", local.port},
                                            {"PublicAddr", local.ip}, {"PublicPort", local.port},
                                            {"MappedAddr", mapped.ip}, {"MappedPort", mapped.port}});
}

void BBHostClient::leave_room(const std::string& session_id, int member_id) {
    call("/mp/matching2/leave_room", {{"SessionId", session_id}, {"MemberId", member_id}});
}

int BBHostClient::heartbeat(const std::string& session_id, int member_id) {
    try {
        json r = call("/mp/matching2/heartbeat", {{"SessionId", session_id}, {"MemberId", member_id}}, 4000);
        return static_cast<int>(jint(r, "InRoom", 1));
    } catch (const BridgeError&) {
        return -1;
    }
}

void BBHostClient::kick_member(const std::string& session_id, int member_id, int kicker_id) {
    call("/mp/matching2/kick_member",
         {{"SessionId", session_id}, {"MemberId", member_id}, {"KickerMemberId", kicker_id}, {"OptData", ""}});
}

void BBHostClient::signaling_update(const std::string& online_id, const Endpoint& mapped,
                                    const std::string& session_id, int member_id) {
    try {
        call("/mp/matching2/signaling_update", {{"OnlineId", online_id}, {"MappedAddr", mapped.ip},
                                                {"MappedPort", mapped.port}, {"SessionId", session_id},
                                                {"MemberId", member_id}});
    } catch (const BridgeError& e) {
        log(std::string("signaling_update failed: ") + e.what());
    }
}

std::optional<Endpoint> BBHostClient::resolve_peer(const std::string& online_id) {
    try {
        json r = call("/np/signaling/resolve", {{"OnlineId", online_id}});
        Endpoint e{jstr(r, "Addr"), static_cast<int>(jint(r, "Port"))};
        if (e.valid()) return e;
    } catch (const BridgeError&) {
    }
    return std::nullopt;
}

uint64_t BBHostClient::poll_events(const std::string& online_id, uint64_t cursor, json& event) {
    Reply r = post("/np/events/poll", {{"OnlineId", online_id}, {"Cursor", cursor}, {"Categories", {"matching2"}}},
                   5000, &poll_pool_);
    if (r.status == 401) throw BridgeError("server refused our token (HTTP 401)");
    if (r.status < 200 || r.status >= 300) throw BridgeError(fmt("/np/events/poll: HTTP %d", r.status));
    event = jint(r.body, "HasEvent", 0) ? r.body : json();
    return static_cast<uint64_t>(jint(r.body, "NextCursor", static_cast<int64_t>(cursor)));
}

void BBHostClient::ack_event(const std::string& online_id, uint64_t cursor) {
    try {
        post("/np/events/ack", {{"OnlineId", online_id}, {"Cursor", cursor}}, 4000);
    } catch (const BridgeError& e) {
        log(std::string("event ack failed: ") + e.what());
    }
}

}  // namespace bb
