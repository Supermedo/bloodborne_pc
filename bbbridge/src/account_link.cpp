#include "account_link.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "http.hpp"

namespace bb {

namespace {

// POST to the account server; the reply is read whatever the status (a limit
// is a 429 whose Message the player should see), as bbhost's auth_call does.
bool auth_call(const std::string& base, const std::string& path, const json& body, json& reply, std::string& error) {
    http::ClientRequest req;
    req.url = base + path;
    req.body = body.dump();
    req.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"},
                   {"User-Agent", std::string("bbbridge/") + kVersion}};
    const http::ClientResponse res = http::send(req);
    if (!res.ok()) {
        error = res.error;
        return false;
    }
    reply = json::parse(res.body, nullptr, false);
    if (reply.is_discarded() || !reply.is_object() || !reply.contains("ResKind")) {
        error = fmt("%s has no account service (HTTP %d)", base.c_str(), res.status);
        return false;
    }
    if (jint(reply, "ResKind", 1) != 0) {
        error = jstr(reply, "Message", "refused");
        return false;
    }
    return true;
}

}  // namespace

namespace {
json read_state(const std::wstring& path) {
    std::ifstream in(path);
    if (!in) return json::object();
    json j = json::parse(in, nullptr, false);
    return j.is_object() ? j : json::object();
}
}  // namespace

std::string account_kind(const std::wstring& state_file, const std::string& npid) {
    const json state = read_state(state_file);
    if (!state.contains("accounts") || !state["accounts"].contains(npid)) return "";
    const json& e = state["accounts"][npid];
    if (jstr(e, "token").empty() && jstr(e, "recovery_code").empty()) return "";
    return jstr(e, "linked") == "discord" ? "discord" : "bbbridge";
}

std::string website_code(const std::string& auth_server, const std::wstring& state_file, const std::string& npid,
                         std::string& error) {
    const json state = read_state(state_file);
    const std::string token =
        state.contains("accounts") && state["accounts"].contains(npid) ? jstr(state["accounts"][npid], "token") : "";
    if (token.empty()) {
        error = "no account for '" + npid + "' yet - play once (or sign in with Discord) first";
        return "";
    }
    json reply;
    if (!auth_call(auth_server, "/auth/web_code", {{"Token", token}}, reply, error)) return "";
    const std::string code = jstr(reply, "Code");
    if (code.empty()) error = "no code in the reply";
    return code;
}

namespace {
bool write_state(const std::wstring& path, const json& state) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << state.dump(2);
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
}  // namespace

std::string recovery_code_of(const std::wstring& state_file, const std::string& name) {
    const json state = read_state(state_file);
    return state.contains("accounts") && state["accounts"].contains(name)
               ? jstr(state["accounts"][name], "recovery_code")
               : "";
}

std::string account_create_now(const std::string& auth_server, const std::wstring& state_file, const std::string& name) {
    json reply;
    std::string error;
    if (!auth_call(auth_server, "/auth/account/create", {{"Name", name}, {"DeviceName", device_name()}}, reply, error))
        return error;
    json state = read_state(state_file);
    if (!state["accounts"].is_object()) state["accounts"] = json::object();
    state["accounts"][name] = {{"bbhost_name", name}, {"recovery_code", jstr(reply, "RecoveryCode")},
                               {"token", jstr(reply, "Token")}, {"online_id", jstr(reply, "OnlineId", name)}};
    if (!write_state(state_file, state)) return "could not write the account file";
    log("account: created '" + name + "' on this PC (recovery code stored in bbbridge_state.json)");
    return "";
}

std::string account_recover_now(const std::string& auth_server, const std::wstring& state_file, const std::string& name,
                                const std::string& recovery_code) {
    json reply;
    std::string error;
    if (!auth_call(auth_server, "/auth/recover", {{"Name", name}, {"RecoveryCode", recovery_code}}, reply, error))
        return error;
    json state = read_state(state_file);
    if (!state["accounts"].is_object()) state["accounts"] = json::object();
    const std::string fresh = jstr(reply, "RecoveryCode");
    state["accounts"][name] = {{"bbhost_name", name}, {"recovery_code", fresh.empty() ? recovery_code : fresh},
                               {"token", jstr(reply, "Token")}, {"online_id", jstr(reply, "OnlineId", name)}};
    if (!write_state(state_file, state)) return "could not write the account file";
    log("account: recovered '" + name + "' (its new recovery code is in bbbridge_state.json)");
    return "";
}

std::string account_sign_out(const std::string& auth_server, const std::wstring& state_file, const std::string& name) {
    json state = read_state(state_file);
    if (!state.contains("accounts") || !state["accounts"].contains(name)) return "no account for '" + name + "' here";
    json& e = state["accounts"][name];
    const std::string token = jstr(e, "token");
    if (!token.empty()) {
        json reply;
        std::string error;
        auth_call(auth_server, "/auth/logout", {{"Token", token}}, reply, error);   // best effort, as bbhost
    }
    if (jstr(e, "recovery_code").empty()) state["accounts"].erase(name);   // Discord: sign in again to come back
    else e.erase("token");
    if (!write_state(state_file, state)) return "could not write the account file";
    log("account: signed out of '" + name + "' on this PC");
    return "";
}

std::string device_name() {
    char host[256] = {};
    size_t n = 0;
    getenv_s(&n, host, sizeof(host), "COMPUTERNAME");
    return n > 1 ? "bbbridge on " + std::string(host).substr(0, 48) : "bbbridge";
}

double DiscordLink::seconds_left() const {
    std::lock_guard<std::mutex> lk(mu_);
    return expires_at_ > 0 ? std::max(0.0, expires_at_ - now()) : 0.0;
}

void DiscordLink::start() {
    cancel();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        user_code_.clear();
        uri_.clear();
        online_id_.clear();
        message_.clear();
        expires_at_ = 0;
    }
    state_ = State::Starting;
    worker_ = std::thread([this] { run(); });
}

void DiscordLink::cancel() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    if (state_ == State::Starting || state_ == State::Waiting) state_ = State::Idle;
}

void DiscordLink::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        message_ = why;
    }
    log("discord sign-in: " + why);
    state_ = State::Failed;
}

void DiscordLink::run() {
    json reply;
    std::string error;
    if (!auth_call(auth_, "/auth/device/start", {{"DeviceName", device_name()}}, reply, error)) return fail(error);
    const std::string code = jstr(reply, "DeviceCode");
    int interval = static_cast<int>(jint(reply, "Interval", 5));
    if (interval < 1) interval = 5;
    const int expires = static_cast<int>(jint(reply, "ExpiresIn", 600));
    {
        std::lock_guard<std::mutex> lk(mu_);
        user_code_ = jstr(reply, "UserCode");
        uri_ = jstr(reply, "VerificationUri");
        expires_at_ = now() + expires;
        if (code.empty() || user_code_.empty()) return fail("no code in the reply");
    }
    log("discord sign-in: approve code " + user_code() + " at " + uri());
    state_ = State::Waiting;
    while (!cancel_) {
        for (int i = 0; i < interval * 10 && !cancel_; ++i) Sleep(100);
        if (cancel_) return;
        if (seconds_left() <= 0) return fail("the code expired - start again");
        if (!auth_call(auth_, "/auth/device/poll", {{"DeviceCode", code}}, reply, error)) return fail(error);
        const std::string status = jstr(reply, "Status");
        if (status == "approved") {
            const std::string token = jstr(reply, "Token"), id = jstr(reply, "OnlineId");
            if (token.empty() || id.empty()) return fail("no token in the reply");
            if (!save(id, token)) return fail("could not write the account file");
            {
                std::lock_guard<std::mutex> lk(mu_);
                online_id_ = id;
            }
            log("discord sign-in: linked, signed in as '" + id + "'");
            state_ = State::Approved;
            return;
        }
        if (status != "pending" && !status.empty()) return fail("the server says: " + status);
    }
}

bool DiscordLink::save(const std::string& online_id, const std::string& token) {
    json state = json::object();
    {
        std::ifstream in(state_file_);
        if (in) {
            state = json::parse(in, nullptr, false);
            if (!state.is_object()) state = json::object();
        }
    }
    if (!state["accounts"].is_object()) state["accounts"] = json::object();
    // Keyed by the account's name, which becomes the NPID: the game signs in to
    // the game server under it, and the token's account matches.
    state["accounts"][online_id] = {{"bbhost_name", online_id}, {"token", token}, {"online_id", online_id},
                                    {"linked", "discord"}};
    const std::wstring tmp = state_file_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << state.dump(2);
    }
    return MoveFileExW(tmp.c_str(), state_file_.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace bb
