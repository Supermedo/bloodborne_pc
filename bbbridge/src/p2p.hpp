// P2P gateway: carries the game's UDP traffic between bbport and bbhost players.
//
// bbport (gpu/shadps4/core/libraries/network/p2p_sockets.cpp)
//     game datagram: [src vport u16 be][dst vport u16 be][payload]
//     dst vport 0xffff: internal "SHAD" packets - 0x05/0x06 echo (17 B),
//     0x07 handshake (62 B), 0x10 control (50 B), 0x21 matching2 handshake (50 B)
// bbhost (src/hle/net.cpp, src/net/stun.cpp)
//     game datagram: [0xff][flags][src][dst][payload] - flags 0xC3 with one-byte
//     vports (Bloodborne's 40 and 30), 0x83 with two-byte, +4 bytes with 0x20
//     hole-punch probe "\xfebbhp\0\0\0"; STUN Binding + BBHOST-HELLO (0x8100);
//     relay: to the STUN port [0xfb]['R'][token 8][port u16][datagram],
//     from it [0xfb]['r'][source port u16][datagram]
//
// One OUTER socket faces the internet and speaks bbhost's format; each remote
// member gets an INNER socket on 127.0.0.1, which is where the port is told the
// member lives. bbport's signaling handshakes are answered on the bbhost
// peer's behalf (bbhost has none: its connection state comes from the server).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util.hpp"

namespace bb::p2p {

// ---- framing (pure, unit-tested) ----
enum class Frame { None, Game, Signaling };
Frame port_to_ps4(const std::string& frame, std::string& out);   // Game: bbhost datagram; Signaling: payload
bool ps4_to_port(const std::string& datagram, std::string& out);
std::string port_signaling_frame(const std::string& payload);
std::string relay_wrap(const std::string& token, int port, const std::string& datagram);
bool relay_unwrap(const std::string& data, int& port, std::string& datagram);
extern const std::string kProbe;

struct StunAnswer {
    Endpoint mapped;
    bool relay = false;
    std::string relay_token;   // 8 bytes
    int relay_port = 0;
};
std::string stun_request(const std::string& token, std::string& txid);   // token "" = zeros
bool stun_parse(const std::string& data, const std::string& txid, StunAnswer& out);

// ---- the port's signaling, answered for one bbhost peer ----
enum : uint8_t { T_ECHO_PING = 0x05, T_ECHO_PONG = 0x06, T_HANDSHAKE = 0x07, T_CONTROL = 0x10, T_M2 = 0x21 };
enum : uint8_t { HS_OFFER = 1, HS_ACCEPT = 2, HS_CHECK = 3, HS_CHECK_ACK = 4 };
enum : uint8_t { CTL_ACTIVATION_REQUEST = 1, CTL_ACTIVATION_ACK = 2, CTL_CLOSE = 3, CTL_ESTABLISHED = 4 };

class PeerSignaling {
public:
    PeerSignaling(const std::string& peer_online_id, int peer_member_id, const std::string& port_online_id,
                  int port_member_id, uint64_t room_id);
    std::vector<std::string> handle(const std::string& payload);   // replies (payloads, no frame header)
    std::vector<std::string> opening() const;                      // what a real peer sends first

    std::string peer_id, port_id;   // 16 bytes each, NUL padded
    int peer_member_id = 0;         // 0 until the peer is a room member
    int port_member_id = 0;
    uint64_t room_id = 0;
    uint32_t conn_id = 0;
    bool sent_established = false;
    bool heard = false;             // any signaling from the port for this peer

private:
    std::string handshake(uint8_t kind, uint32_t to_conn, uint64_t nonce) const;
    std::string control(uint8_t kind) const;
    std::string m2(uint8_t kind, uint64_t nonce) const;
};

// ---- the gateway ----
struct PeerInfo {   // a snapshot, for the app and the tests
    int member_id = 0;
    std::string online_id;
    Endpoint endpoint, inner;
    int peer_member_id = 0, port_member_id = 0;
    uint64_t room_id = 0;
    uint64_t to_peer = 0, to_port = 0, signaling = 0;
};

class Gateway {
public:
    Gateway(int outer_port, std::optional<std::pair<std::string, int>> stun_server, std::string lan_addr,
            std::function<void(const Endpoint&)> on_mapped_change);
    ~Gateway();
    bool start(std::string* error);
    void stop();

    std::pair<Endpoint, Endpoint> endpoints() const;   // (local, mapped) as bbhost registers them
    std::optional<Endpoint> mapped() const;
    int relay_port() const;

    // Returns the 127.0.0.1 endpoint the port should use for this member.
    Endpoint add_peer(int member_id, const std::string& online_id, const Endpoint& endpoint,
                      const Endpoint& local_endpoint, const std::string& port_online_id, int port_member_id,
                      uint64_t room_id, const std::optional<Endpoint>& port_endpoint);
    void remove_peer(int member_id);
    void clear_peers();
    void remove_peers_except(const std::set<std::string>& online_ids);
    std::optional<Endpoint> inner_endpoint_for(const std::string& online_id) const;
    std::vector<PeerInfo> peers() const;

    double punch_interval = 0.5, punch_for = 20.0, opening_interval = 1.0, opening_for = 30.0,
           stun_refresh = 15.0;

private:
    struct Link {
        int member_id = 0;
        std::string online_id;
        Endpoint endpoint, local_endpoint;
        std::set<Endpoint> aliases;
        std::optional<Endpoint> port_endpoint;
        PeerSignaling signaling;
        SOCKET inner = INVALID_SOCKET;
        Endpoint inner_endpoint;
        bool heard_peer = false;
        double created = 0;
        uint64_t to_peer = 0, to_port = 0, signaling_count = 0;
        Link(PeerSignaling s) : signaling(std::move(s)) {}
    };
    using LinkPtr = std::shared_ptr<Link>;

    void outer_main();
    void inner_main();
    void tick_main();
    void send_outer(const std::string& datagram, const Endpoint& to);
    void to_port(const LinkPtr& link, const std::string& frame);
    LinkPtr link_for(const Endpoint& src);   // under mu_
    bool relay_target(const Endpoint& e) const;   // under mu_
    void send_stun();
    void on_stun(const std::string& data);
    static void close_link(Link& link);

    int outer_port_ = 0;
    std::optional<std::pair<std::string, int>> stun_server_;
    std::string lan_addr_;
    std::function<void(const Endpoint&)> on_mapped_change_;
    SOCKET outer_ = INVALID_SOCKET;
    mutable std::mutex mu_;
    std::map<int, LinkPtr> peers_;
    std::optional<Endpoint> mapped_;
    struct Relay {
        Endpoint server;   // its STUN endpoint
        std::string token;
        int port = 0;      // ours on it
    };
    std::optional<Relay> relay_;
    std::string stun_txid_;
    Endpoint stun_server_ep_;
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};

}  // namespace bb::p2p
