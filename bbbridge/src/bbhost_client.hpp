// The Hunter's Dream (bbhost server) REST client. The contract is bbhost's own
// client code (GPL-3.0): src/net/http.cpp (headers, ResKind), src/net/account.cpp,
// src/net/session.cpp, src/hle/np_matching2.cpp.
#pragma once

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "http.hpp"
#include "util.hpp"

namespace bb {

struct BridgeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BBHostClient {
public:
    explicit BBHostClient(std::string base) : base_(std::move(base)) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
    }

    std::string user_agent = "bbbridge";
    std::string ruleset = "vanilla";

    std::string token() const { std::lock_guard<std::mutex> lk(mu_); return token_; }
    std::string online_id() const { std::lock_guard<std::mutex> lk(mu_); return online_id_; }
    void set_identity(const std::string& token, const std::string& online_id) {
        std::lock_guard<std::mutex> lk(mu_);
        token_ = token;
        online_id_ = online_id;
    }

    // account; each returns the recovery code the server sent ("" if none)
    std::string account_create(const std::string& name);
    std::string account_recover(const std::string& name, const std::string& code);
    // true accepted, false refused (401), nullopt when it could not be asked
    std::optional<bool> token_valid(const std::string& online_id);

    // matching
    json context_start(const std::string& online_id, const Endpoint& signaling, const Endpoint& mapped);
    json create_room(const std::string& online_id, const Endpoint& local, const Endpoint& mapped, int max_members,
                     const json& extra);
    json join_room(const std::string& online_id, uint64_t room_id, const Endpoint& local, const Endpoint& mapped);
    void leave_room(const std::string& session_id, int member_id);
    int heartbeat(const std::string& session_id, int member_id);    // InRoom, -1 when not asked
    void kick_member(const std::string& session_id, int member_id, int kicker_id);
    void signaling_update(const std::string& online_id, const Endpoint& mapped, const std::string& session_id,
                          int member_id);
    std::optional<Endpoint> resolve_peer(const std::string& online_id);

    // events: returns the next cursor; `event` is null when there was none
    uint64_t poll_events(const std::string& online_id, uint64_t cursor, json& event);
    void ack_event(const std::string& online_id, uint64_t cursor);

private:
    struct Reply {
        int status = 0;
        json body;
    };
    // throws on transport failure; pool: a connection of its own (else the shared one)
    Reply post(const std::string& path, const json& body, int timeout_ms = 8000, http::Pool* pool = nullptr);
    json call(const std::string& path, const json& body, int timeout_ms = 8000);     // + ResKind == 0

    http::Pool poll_pool_{45.0};   // the event poll, every 0.4 s: redialled before it is a minute old
    std::string base_;
    mutable std::mutex mu_;
    std::string token_;
    std::string online_id_;
};

}  // namespace bb
