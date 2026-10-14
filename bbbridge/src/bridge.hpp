// bbbridge core: a local shadNet server for bbport that speaks The Hunter's
// Dream's REST API (bbhost's protocol) behind it, plus the P2P gateway, the
// WebAPI the game's summon calls go to, and the game-server proxy.
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bbhost_client.hpp"
#include "http.hpp"
#include "p2p.hpp"
#include "util.hpp"

namespace bb {

uint64_t account_id_for(const std::string& npid);   // the PSN-style id bbport hands the game

struct Config {
    std::string bbhost_server = "https://thehuntersdream.com:18671";
    std::string auth_server = "https://thehuntersdream.com";   // Discord sign-in, website codes (bbhost's auth_server)
    std::string community_server = "https://thehuntersdream.com:18671";
    std::string bind_host = "127.0.0.1";
    int tcp_port = 31313, udp_port = 31314, webapi_port = 31315;
    bool p2p_gateway = true;
    int p2p_port = 0;               // outer UDP port, 0 = any
    int port_p2p_port = 3658;       // bbport's P2P UDP port (SHADPS4_P2P_PORT)
    std::string stun_server;        // "host:port", "" = the bbhost host on 3478, "off"
    std::string local_addr;         // LAN address to register, "" = detected
    double poll_interval = 0.4;
    std::string state_file = "bbbridge_state.json";
    bool trace = false;
    bool debug_endpoints = false;   // /debug/* on the WebAPI (tests only)
    bool create_accounts = false;   // make an account for an unknown NPID (tests); players sign in with Discord
    double stun_refresh = 15.0;     // seconds between STUN refreshes (tests shorten it)
    std::map<int, std::string> game_proxy_routes = {{31316, "https://thehuntersdream.com:20443"},
                                                    {31317, "https://thehuntersdream.com:18671"}};
    std::string user_agent = std::string("bbbridge/") + kVersion;

    static Config from_json(const json& j);
    std::optional<std::pair<std::string, int>> stun_endpoint() const;
};

class Bridge;

class TcpConn {
public:
    explicit TcpConn(SOCKET s) : sock(s) {}
    bool send(const std::string& data);
    void close();
    SOCKET sock;
    std::atomic<bool> alive{true};

private:
    std::mutex send_mu_;
};

struct RoomState {
    uint64_t room_id = 0;
    std::string session_id;
    int member_id = 0, owner_id = 0, max_slot = 5;
    std::map<int, json> members;   // member_id -> {OnlineId, Addr, Port, LocalAddr, LocalPort, MappedAddr, MappedPort}
    void clear() { *this = RoomState{}; }
};

class Session {
public:
    Session(Bridge& bridge, std::shared_ptr<TcpConn> conn);
    ~Session();
    std::pair<uint8_t, std::string> handle(uint16_t command, uint64_t packet_id, const std::string& payload);
    void shutdown();

    bool logged_in() const { return logged_in_; }
    std::string npid() const;
    BBHostClient bb;
    std::atomic<bool> terminate_requested{false};
    std::shared_ptr<TcpConn> conn;

    // for the bridge (mapped-address change, summon advertisement, app status)
    void on_mapped_change(const Endpoint& mapped);
    void set_advertisement(const json& adv);
    uint64_t room_id() const;
    int room_members() const;

private:
    using Reply = std::pair<uint8_t, std::string>;
    Reply on_login(const std::string& payload);
    Reply on_get_token();
    Reply on_context_start(const std::string& payload);
    Reply on_context_stop();
    Reply on_get_world_info_list();
    Reply on_create_room(const std::string& payload);
    Reply on_join_room(const std::string& payload);
    Reply on_leave_room(const std::string& payload);
    Reply on_request_signaling_infos(const std::string& payload);
    Reply on_room_id_echo(const std::string& payload);   // SetRoomData{Internal,External}
    Reply on_kickout(const std::string& payload);

    bool bbhost_account();
    std::pair<Endpoint, Endpoint> p2p_addresses();
    json host_extra_from_advertisement() const;
    static Endpoint member_endpoint(const json& member);
    Endpoint port_endpoint_for(const json& member);
    std::optional<Endpoint> port_p2p_endpoint() const;
    void forget_peers(std::optional<int> member_id = std::nullopt);
    void ensure_self_member();
    void absorb_members(const json& members);
    std::string build_details(int me_id, int owner_id);
    std::string member_data(const json& member, int owner_id);
    void leave_room_quietly();
    void send_room_event(uint32_t event, uint32_t cause, const json& member, int64_t error_code = 0);
    void start_pump();
    void pump_main();
    void handle_event(const json& event);

    Bridge& bridge_;
    mutable std::recursive_mutex mu_;   // the session's state; the TCP and pump threads share it
    std::string npid_, title_id_;
    uint64_t ctx_id_ = 0;
    std::atomic<bool> logged_in_{false};
    uint64_t account_id_ = 0;
    RoomState room_;
    json advertisement_;
    std::vector<json> pending_guest_invites_;
    std::thread pump_;
    std::atomic<bool> pump_running_{false};
    std::mutex join_mu_;   // shutdown() can come from the TCP thread and Bridge::stop at once
    std::mutex pump_mu_;
    std::condition_variable pump_cv_;
    bool pump_stop_ = false;
};

struct BridgeStatus {
    bool running = false;
    std::string npid;
    bool logged_in = false;
    uint64_t room_id = 0;
    int room_members = 0;
    int relay_port = 0;
    std::optional<Endpoint> mapped;
    std::vector<p2p::PeerInfo> peers;
};

class Bridge {
public:
    explicit Bridge(Config cfg) : cfg(std::move(cfg)) {}
    ~Bridge() { stop(); }
    bool start(std::string* error);
    void stop();
    BridgeStatus status() const;

    const Config cfg;
    std::unique_ptr<p2p::Gateway> gateway;

    // shared with sessions
    json load_state();
    void save_state(const json& state);
    std::optional<Endpoint> registry_get(const std::string& npid) const;
    std::pair<std::string, std::string> identity() const;   // (token, ruleset) of the logged-in player
    json last_advertisement() const;

private:
    void tcp_accept_loop();
    void tcp_serve(std::shared_ptr<TcpConn> conn);
    void stun_loop();
    http::ServerResponse webapi(const http::ServerRequest& req);
    http::ServerResponse summon(const http::ServerRequest& req);
    http::ServerResponse game_proxy(const std::string& upstream, const http::ServerRequest& req);
    void capture(const std::string& name, const std::string& request, int status, const std::string& response,
                 const std::string& note);
    void on_mapped_change(const Endpoint& mapped);

    SOCKET tcp_ = INVALID_SOCKET, stun_ = INVALID_SOCKET;
    std::thread tcp_thread_, stun_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> tcp_active_{0};
    http::Server webapi_server_;
    std::vector<std::unique_ptr<http::Server>> proxies_;

    mutable std::mutex sessions_mu_;
    std::vector<std::shared_ptr<Session>> sessions_;
    mutable std::mutex registry_mu_;
    std::map<std::string, Endpoint> registry_;   // npid -> the port's endpoint, from its STUN pings
    json last_advertisement_;
    std::mutex state_mu_;
};

}  // namespace bb
