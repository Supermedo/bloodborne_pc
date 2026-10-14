// Sign in with Discord: the OAuth device flow bbhost uses (src/net/account.cpp).
//   POST /auth/device/start {DeviceName}  -> DeviceCode, UserCode, VerificationUri, Interval, ExpiresIn
//   POST /auth/device/poll  {DeviceCode}  -> Status pending | approved (+ Token, OnlineId) | ...
// The player approves the code on The Hunter's Dream with their Discord account;
// the account and its token then go into bbbridge_state.json under its name, and
// the bridge signs in with that token when the game connects with that NPID.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "util.hpp"

namespace bb {

class DiscordLink {
public:
    enum class State { Idle, Starting, Waiting, Approved, Failed };

    DiscordLink(std::string auth_server, std::wstring state_file) : auth_(std::move(auth_server)), state_file_(std::move(state_file)) {}
    ~DiscordLink() { cancel(); }

    void start();    // runs in its own thread; read progress with the getters
    void cancel();

    State state() const { return state_; }
    std::string user_code() const { std::lock_guard<std::mutex> lk(mu_); return user_code_; }
    std::string uri() const { std::lock_guard<std::mutex> lk(mu_); return uri_; }
    std::string online_id() const { std::lock_guard<std::mutex> lk(mu_); return online_id_; }
    std::string message() const { std::lock_guard<std::mutex> lk(mu_); return message_; }
    double seconds_left() const;

private:
    void run();
    void fail(const std::string& why);
    bool save(const std::string& online_id, const std::string& token);

    std::string auth_;
    std::wstring state_file_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mu_;
    std::string user_code_, uri_, online_id_, message_;
    double expires_at_ = 0;
    std::thread worker_;
};

std::string device_name();   // "bbbridge on <COMPUTERNAME>"

// The website sign-in code for an account the bridge holds a token for
// (thehuntersdream.com/account, "Sign in with a code from the game"):
// POST /auth/web_code {Token} -> Code. Returns "" and sets error on failure.
std::string website_code(const std::string& auth_server, const std::wstring& state_file, const std::string& npid,
                         std::string& error);

// What the state file holds for this NPID: "discord", "bbbridge", or "" (none yet).
std::string account_kind(const std::wstring& state_file, const std::string& npid);

// bbhost's other account buttons (src/net/account.cpp), on the same server:
// create an account named `name` on this PC; recover one with its recovery code
// (the server answers with a new code, which replaces the old one); sign out
// (the token is ended on the server and removed here; a recovery code stays).
// Each returns "" on success, else the reason.
std::string account_create_now(const std::string& auth_server, const std::wstring& state_file, const std::string& name);
std::string account_recover_now(const std::string& auth_server, const std::wstring& state_file, const std::string& name,
                                const std::string& recovery_code);
std::string account_sign_out(const std::string& auth_server, const std::wstring& state_file, const std::string& name);
std::string recovery_code_of(const std::wstring& state_file, const std::string& name);

}  // namespace bb
