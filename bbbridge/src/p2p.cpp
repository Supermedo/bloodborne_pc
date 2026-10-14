#include "p2p.hpp"

#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <chrono>
#include <cstring>
#include <random>

namespace bb::p2p {

namespace {

constexpr uint16_t kPortSignalingVport = 0xFFFF;
constexpr uint8_t kPs4Magic = 0xFF, kFlagP2p = 0x80, kFlagByteVports = 0x40, kFlagComid = 0x20, kFlagType = 0x03;
constexpr uint8_t kRelayMagic = 0xFB;
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr uint16_t kStunBindingRequest = 0x0001, kStunBindingResponse = 0x0101, kStunMapped = 0x0001,
                   kStunXorMapped = 0x0020, kStunXorMappedOld = 0x8020, kBbhostHello = 0x8100,
                   kBbhostRelay = 0x8101;
const std::string kRelayVersion = "bbr1";
const std::string kShad = "SHAD";

uint16_t be16(const std::string& s, size_t at) {
    return static_cast<uint16_t>((static_cast<uint8_t>(s[at]) << 8) | static_cast<uint8_t>(s[at + 1]));
}
uint32_t be32(const std::string& s, size_t at) {
    return (static_cast<uint32_t>(be16(s, at)) << 16) | be16(s, at + 2);
}
void put_be16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v & 0xFF));
}
template <typename T>
void put_le(std::string& s, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) s.push_back(static_cast<char>((static_cast<uint64_t>(v) >> (8 * i)) & 0xFF));
}
template <typename T>
T get_le(const std::string& s, size_t at) {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[at + i])) << (8 * i);
    return static_cast<T>(v);
}
std::string id16(const std::string& name) {
    std::string out = name.substr(0, 16);
    out.resize(16, '\0');
    return out;
}
std::string ip_from_be32(uint32_t v) {
    return fmt("%u.%u.%u.%u", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}
uint64_t wall_micros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}
void no_udp_connreset(SOCKET s) {
    // Without this, an ICMP "port unreachable" for an earlier send makes the
    // next recvfrom fail with WSAECONNRESET (10054).
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
}
SOCKET udp_bind(const std::string& ip, int port, Endpoint* bound) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a = to_sockaddr(Endpoint{ip, port});
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    no_udp_connreset(s);
    int len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    if (bound) *bound = from_sockaddr(a);
    return s;
}
void udp_send(SOCKET s, const std::string& data, const Endpoint& to) {
    sockaddr_in a = to_sockaddr(to);
    sendto(s, data.data(), static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
}

}  // namespace

const std::string kProbe("\xfe" "bbhp\0\0\0", 8);

// ---- framing ----

Frame port_to_ps4(const std::string& frame, std::string& out) {
    if (frame.size() < 4) return Frame::None;
    const uint16_t src = be16(frame, 0), dst = be16(frame, 2);
    const std::string payload = frame.substr(4);
    if (dst == kPortSignalingVport) {
        out = payload;
        return Frame::Signaling;
    }
    out.clear();
    out.push_back(static_cast<char>(kPs4Magic));
    if (src < 256 && dst < 256) {
        out.push_back(static_cast<char>(kFlagP2p | kFlagByteVports | kFlagType));
        out.push_back(static_cast<char>(src));
        out.push_back(static_cast<char>(dst));
    } else {
        out.push_back(static_cast<char>(kFlagP2p | kFlagType));
        put_be16(out, src);
        put_be16(out, dst);
    }
    out += payload;
    return Frame::Game;
}

bool ps4_to_port(const std::string& d, std::string& out) {
    if (d.size() < 4 || static_cast<uint8_t>(d[0]) != kPs4Magic) return false;
    const uint8_t flags = static_cast<uint8_t>(d[1]);
    size_t at = 2 + ((flags & kFlagComid) ? 4 : 0);
    uint16_t src, dst;
    if (flags & kFlagByteVports) {
        if (at + 2 > d.size()) return false;
        src = static_cast<uint8_t>(d[at]);
        dst = static_cast<uint8_t>(d[at + 1]);
        at += 2;
    } else {
        if (at + 4 > d.size()) return false;
        src = be16(d, at);
        dst = be16(d, at + 2);
        at += 4;
    }
    out.clear();
    put_be16(out, src);
    put_be16(out, dst);
    out += d.substr(at);
    return true;
}

std::string port_signaling_frame(const std::string& payload) {
    std::string out;
    put_be16(out, kPortSignalingVport);
    put_be16(out, kPortSignalingVport);
    return out + payload;
}

std::string relay_wrap(const std::string& token, int port, const std::string& datagram) {
    std::string out;
    out.push_back(static_cast<char>(kRelayMagic));
    out.push_back('R');
    out += token;
    put_be16(out, static_cast<uint16_t>(port));
    return out + datagram;
}

bool relay_unwrap(const std::string& data, int& port, std::string& datagram) {
    if (data.size() < 4 || static_cast<uint8_t>(data[0]) != kRelayMagic || data[1] != 'r') return false;
    port = be16(data, 2);
    datagram = data.substr(4);
    return true;
}

std::string stun_request(const std::string& token, std::string& txid) {
    static std::mt19937_64 rng{std::random_device{}()};
    txid.clear();
    txid.push_back(static_cast<char>(kStunCookie >> 24));
    txid.push_back(static_cast<char>((kStunCookie >> 16) & 0xFF));
    txid.push_back(static_cast<char>((kStunCookie >> 8) & 0xFF));
    txid.push_back(static_cast<char>(kStunCookie & 0xFF));
    for (int i = 0; i < 12; ++i) txid.push_back(static_cast<char>(rng() & 0xFF));
    std::string attr;
    put_be16(attr, kBbhostHello);
    put_be16(attr, 12);
    attr += kRelayVersion;
    attr += token.size() == 8 ? token : std::string(8, '\0');
    std::string out;
    put_be16(out, kStunBindingRequest);
    put_be16(out, static_cast<uint16_t>(attr.size()));
    return out + txid + attr;
}

bool stun_parse(const std::string& d, const std::string& txid, StunAnswer& out) {
    if (d.size() < 20 || be16(d, 0) != kStunBindingResponse || d.compare(4, 16, txid) != 0) return false;
    const size_t length = be16(d, 2);
    if (20 + length > d.size()) return false;
    const uint32_t xor_addr = be32(d, 4);
    const uint16_t xor_port = be16(d, 4);
    std::optional<std::pair<uint32_t, uint16_t>> mapped, xored;
    out = StunAnswer{};
    size_t at = 20;
    const size_t end = 20 + length;
    while (at + 4 <= end) {
        const uint16_t type = be16(d, at), alen = be16(d, at + 2);
        if (at + 4 + alen > end) break;
        const std::string v = d.substr(at + 4, alen);
        if (type == kBbhostRelay && alen >= 20 && v.compare(0, 4, kRelayVersion) == 0) {
            out.relay = true;
            out.relay_token = v.substr(4, 8);
            out.relay_port = be16(v, 12);
        } else if (alen >= 8 && static_cast<uint8_t>(v[1]) == 0x01) {
            const uint16_t port = be16(v, 2);
            const uint32_t addr = be32(v, 4);
            if (type == kStunMapped) mapped = std::make_pair(addr, port);
            else if (type == kStunXorMapped || type == kStunXorMappedOld)
                xored = std::make_pair(addr ^ xor_addr, static_cast<uint16_t>(port ^ xor_port));
        }
        at += 4 + ((alen + 3u) & ~3u);
    }
    const auto chosen = mapped ? mapped : xored;
    if (!chosen || !chosen->first || !chosen->second) return false;
    out.mapped = Endpoint{ip_from_be32(chosen->first), chosen->second};
    return true;
}

// ---- signaling ----

PeerSignaling::PeerSignaling(const std::string& peer_online_id, int peer_member, const std::string& port_online_id,
                             int port_member, uint64_t room)
    : peer_id(id16(peer_online_id)),
      port_id(id16(port_online_id)),
      peer_member_id(peer_member > 0 ? peer_member : 0),
      port_member_id(port_member),
      room_id(room),
      conn_id(0x4242u + static_cast<uint32_t>(peer_member > 0 ? peer_member : 0)) {}

std::string PeerSignaling::handshake(uint8_t kind, uint32_t to_conn, uint64_t nonce) const {
    std::string p = kShad;
    p.push_back(static_cast<char>(T_HANDSHAKE));
    p.push_back(static_cast<char>(kind));
    p += peer_id + port_id;
    put_le<uint32_t>(p, conn_id);
    put_le<uint32_t>(p, to_conn);
    put_le<uint32_t>(p, 0);   // mapped addr: 0 makes the port keep the inner socket's address
    put_le<uint16_t>(p, 0);
    put_le<uint16_t>(p, 0);
    put_le<uint64_t>(p, nonce);
    return p;   // 62 bytes
}

std::string PeerSignaling::control(uint8_t kind) const {
    std::string p = kShad;
    p.push_back(static_cast<char>(T_CONTROL));
    p.push_back(static_cast<char>(kind));
    p += peer_id + port_id;
    put_le<uint32_t>(p, conn_id);
    put_le<uint32_t>(p, 0);
    put_le<uint16_t>(p, 0);
    put_le<uint16_t>(p, 0);
    return p;   // 50 bytes
}

std::string PeerSignaling::m2(uint8_t kind, uint64_t nonce) const {
    std::string p = kShad;
    p.push_back(static_cast<char>(T_M2));
    p.push_back(static_cast<char>(kind));
    put_le<uint64_t>(p, room_id);
    put_le<uint16_t>(p, static_cast<uint16_t>(peer_member_id));
    put_le<uint16_t>(p, static_cast<uint16_t>(port_member_id));
    p += peer_id;
    put_le<uint32_t>(p, 0);
    put_le<uint16_t>(p, 0);
    put_le<uint16_t>(p, 0);
    put_le<uint64_t>(p, nonce);
    return p;   // 50 bytes
}

std::vector<std::string> PeerSignaling::handle(const std::string& p) {
    if (p.size() < 5 || p.compare(0, 4, kShad) != 0) return {};
    const uint8_t type = static_cast<uint8_t>(p[4]);
    if (type == T_ECHO_PING && p.size() == 17) {
        heard = true;
        std::string pong = kShad;
        pong.push_back(static_cast<char>(T_ECHO_PONG));
        pong += p.substr(5, 12);   // conn u32 + timestamp u64, echoed
        return {pong};
    }
    if (type == T_HANDSHAKE && p.size() == 0x3E) {
        heard = true;
        const uint8_t kind = static_cast<uint8_t>(p[5]);
        const uint32_t from_conn = get_le<uint32_t>(p, 38);
        const uint64_t nonce = get_le<uint64_t>(p, 54);
        if (kind == HS_OFFER) return {handshake(HS_ACCEPT, from_conn, 0)};
        if (kind == HS_ACCEPT) return {handshake(HS_CHECK, from_conn, wall_micros())};
        if (kind == HS_CHECK) return {handshake(HS_CHECK_ACK, from_conn, nonce)};
        return {};
    }
    if (type == T_CONTROL && p.size() == 0x32) {
        heard = true;
        const uint8_t kind = static_cast<uint8_t>(p[5]);
        if (kind == CTL_ACTIVATION_REQUEST) return {control(CTL_ACTIVATION_ACK)};
        if (kind == CTL_ESTABLISHED && !sent_established) {
            sent_established = true;
            return {control(CTL_ESTABLISHED)};
        }
        return {};
    }
    if (type == T_M2 && p.size() == 0x32) {
        heard = true;
        const uint8_t kind = static_cast<uint8_t>(p[5]);
        const uint64_t room = get_le<uint64_t>(p, 6);
        const uint16_t from_member = get_le<uint16_t>(p, 14), to_member = get_le<uint16_t>(p, 16);
        const uint64_t nonce = get_le<uint64_t>(p, 42);
        if (room) room_id = room;
        // the port addresses the member it means: learn the ids from it
        if (to_member && !peer_member_id) peer_member_id = to_member;
        if (from_member && !port_member_id) port_member_id = from_member;
        if (kind == HS_OFFER) return {m2(HS_ACCEPT, 0)};
        if (kind == HS_ACCEPT) return {m2(HS_CHECK, 0)};
        if (kind == HS_CHECK) return {m2(HS_CHECK_ACK, nonce)};
        return {};
    }
    return {};
}

std::vector<std::string> PeerSignaling::opening() const {
    std::vector<std::string> out{control(CTL_ACTIVATION_REQUEST)};
    if (room_id && port_member_id && peer_member_id) out.push_back(m2(HS_OFFER, 0));
    return out;
}

// ---- gateway ----

Gateway::Gateway(int outer_port, std::optional<std::pair<std::string, int>> stun_server, std::string lan_addr,
                 std::function<void(const Endpoint&)> on_mapped_change)
    : outer_port_(outer_port),
      stun_server_(std::move(stun_server)),
      lan_addr_(std::move(lan_addr)),
      on_mapped_change_(std::move(on_mapped_change)) {}

Gateway::~Gateway() { stop(); }

bool Gateway::start(std::string* error) {
    net_init();
    Endpoint bound;
    outer_ = udp_bind("0.0.0.0", outer_port_, &bound);
    if (outer_ == INVALID_SOCKET) {
        if (error) *error = fmt("p2p: cannot bind udp :%d (error %d)", outer_port_, WSAGetLastError());
        return false;
    }
    outer_port_ = bound.port;
    stop_ = false;
    threads_.emplace_back([this] { outer_main(); });
    threads_.emplace_back([this] { inner_main(); });
    threads_.emplace_back([this] { tick_main(); });
    log(fmt("p2p: gateway on udp :%d (bbhost framing)", outer_port_));
    return true;
}

void Gateway::stop() {
    if (stop_.exchange(true)) return;
    if (outer_ != INVALID_SOCKET) closesocket(outer_);
    outer_ = INVALID_SOCKET;
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, link] : peers_) close_link(*link);
    peers_.clear();
}

void Gateway::close_link(Link& link) {
    if (link.inner != INVALID_SOCKET) closesocket(link.inner);
    link.inner = INVALID_SOCKET;
}

std::pair<Endpoint, Endpoint> Gateway::endpoints() const {
    std::lock_guard<std::mutex> lk(mu_);
    const Endpoint local{lan_addr_, outer_port_};
    return {local, mapped_ ? *mapped_ : local};
}

std::optional<Endpoint> Gateway::mapped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return mapped_;
}

int Gateway::relay_port() const {
    std::lock_guard<std::mutex> lk(mu_);
    return relay_ ? relay_->port : 0;
}

Endpoint Gateway::add_peer(int member_id, const std::string& online_id, const Endpoint& endpoint,
                           const Endpoint& local_endpoint, const std::string& port_online_id, int port_member_id,
                           uint64_t room_id, const std::optional<Endpoint>& port_endpoint) {
    LinkPtr link;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // A peer the port asked for before it joined (a host signaling the
        // guest it summons) sits under a provisional key: keep its inner socket
        // - the port knows it there - and file it under the real id.
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (!online_id.empty() && it->second->online_id == online_id && it->first != member_id) {
                LinkPtr other = it->second;
                peers_.erase(it);
                auto old = peers_.find(member_id);
                if (old != peers_.end()) {
                    if (old->second != other) close_link(*old->second);
                    peers_.erase(old);
                }
                other->member_id = member_id;
                other->signaling.peer_member_id = member_id > 0 ? member_id : 0;
                peers_[member_id] = other;
                break;
            }
        }
        auto it = peers_.find(member_id);
        if (it != peers_.end() && it->second->online_id == online_id) {
            Link& l = *it->second;
            if (endpoint.valid()) l.endpoint = endpoint;
            if (local_endpoint.valid()) l.local_endpoint = local_endpoint;
            if (port_endpoint && !l.port_endpoint) l.port_endpoint = port_endpoint;
            // a guest invite seeds the host before the port has joined: the room
            // and the port's member id arrive with the join
            if (port_member_id) l.signaling.port_member_id = port_member_id;
            if (room_id) l.signaling.room_id = room_id;
            return l.inner_endpoint;
        }
        if (it != peers_.end()) {
            close_link(*it->second);
            peers_.erase(it);
        }
        link = std::make_shared<Link>(PeerSignaling(online_id, member_id, port_online_id, port_member_id, room_id));
        link->member_id = member_id;
        link->online_id = online_id;
        link->endpoint = endpoint;
        link->local_endpoint = local_endpoint;
        link->port_endpoint = port_endpoint;
        link->created = now();
        link->inner = udp_bind("127.0.0.1", 0, &link->inner_endpoint);
        peers_[member_id] = link;
    }
    log(fmt("p2p: member %d (%s) at %s -> port sees 127.0.0.1:%d", member_id, online_id.c_str(),
            endpoint.valid() ? endpoint.str().c_str() : "?:0", link->inner_endpoint.port));
    return link->inner_endpoint;
}

void Gateway::remove_peer(int member_id) {
    LinkPtr link;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = peers_.find(member_id);
        if (it == peers_.end()) return;
        link = it->second;
        peers_.erase(it);
        close_link(*link);
    }
    log(fmt("p2p: member %d removed (to peer %llu, to port %llu, signaling %llu)", member_id,
            static_cast<unsigned long long>(link->to_peer), static_cast<unsigned long long>(link->to_port),
            static_cast<unsigned long long>(link->signaling_count)));
}

void Gateway::clear_peers() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [id, link] : peers_) ids.push_back(id);
    }
    for (int id : ids) remove_peer(id);
}

void Gateway::remove_peers_except(const std::set<std::string>& online_ids) {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [id, link] : peers_)
            if (!online_ids.count(link->online_id)) ids.push_back(id);
    }
    for (int id : ids) remove_peer(id);
}

std::optional<Endpoint> Gateway::inner_endpoint_for(const std::string& online_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, link] : peers_)
        if (link->online_id == online_id) return link->inner_endpoint;
    return std::nullopt;
}

std::vector<PeerInfo> Gateway::peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerInfo> out;
    for (const auto& [id, l] : peers_) {
        PeerInfo p;
        p.member_id = id;
        p.online_id = l->online_id;
        p.endpoint = l->endpoint;
        p.inner = l->inner_endpoint;
        p.peer_member_id = l->signaling.peer_member_id;
        p.port_member_id = l->signaling.port_member_id;
        p.room_id = l->signaling.room_id;
        p.to_peer = l->to_peer;
        p.to_port = l->to_port;
        p.signaling = l->signaling_count;
        out.push_back(p);
    }
    return out;
}

bool Gateway::relay_target(const Endpoint& e) const {
    return relay_ && e.valid() && e.ip == relay_->server.ip && e.port != relay_->server.port;
}

void Gateway::send_outer(const std::string& datagram, const Endpoint& to) {
    if (!to.valid()) return;
    std::optional<Relay> relay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        relay = relay_;
    }
    if (relay && to.ip == relay->server.ip && to.port != relay->server.port)
        udp_send(outer_, relay_wrap(relay->token, to.port, datagram), relay->server);
    else
        udp_send(outer_, datagram, to);
}

void Gateway::to_port(const LinkPtr& link, const std::string& frame) {
    SOCKET s;
    std::optional<Endpoint> dst;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = link->inner;
        dst = link->port_endpoint;
    }
    if (s != INVALID_SOCKET && dst) udp_send(s, frame, *dst);
}

Gateway::LinkPtr Gateway::link_for(const Endpoint& src) {
    for (auto& [id, link] : peers_) {
        if (src == link->endpoint || src == link->local_endpoint || link->aliases.count(src)) {
            if (src != link->endpoint && link->aliases.count(src)) link->endpoint = src;   // answer where it last spoke
            return link;
        }
    }
    // A NAT may hand the peer's datagrams a new port, and a relayed bbhost peer
    // can reach us from two relay ports (the one the room lists and the one
    // signaling resolved). With one peer from that address, take the new port
    // as another of its endpoints.
    LinkPtr only;
    int same = 0;
    for (auto& [id, link] : peers_)
        if (link->endpoint.valid() && link->endpoint.ip == src.ip) {
            only = link;
            ++same;
        }
    if (same != 1) return nullptr;
    only->aliases.insert(only->endpoint);
    only->aliases.insert(src);
    log(fmt("p2p: member %d (%s) also at %s", only->member_id, only->online_id.c_str(), src.str().c_str()));
    only->endpoint = src;
    return only;
}

void Gateway::outer_main() {
    std::string buf(65536, '\0');
    while (!stop_) {
        sockaddr_in from{};
        int flen = sizeof(from);
        const int n = recvfrom(outer_, buf.data(), static_cast<int>(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) {
            if (stop_) return;
            continue;
        }
        Endpoint src = from_sockaddr(from);
        std::string data(buf.data(), static_cast<size_t>(n));
        {
            std::lock_guard<std::mutex> lk(mu_);
            int port;
            std::string inner;
            if (relay_ && src == relay_->server && relay_unwrap(data, port, inner)) {
                src = Endpoint{relay_->server.ip, port};
                data = std::move(inner);
            }
        }
        if (data.size() >= 20 && data[0] == 0x01 && data[1] == 0x01) {
            on_stun(data);
            continue;
        }
        LinkPtr link;
        {
            std::lock_guard<std::mutex> lk(mu_);
            link = link_for(src);
            if (data.compare(0, 5, kProbe, 0, 5) == 0) {
                if (link) link->heard_peer = true;
                continue;
            }
        }
        std::string frame;
        if (!link || !ps4_to_port(data, frame)) continue;
        {
            std::lock_guard<std::mutex> lk(mu_);
            link->heard_peer = true;
            ++link->to_port;
        }
        to_port(link, frame);
    }
}

void Gateway::inner_main() {
    std::string buf(65536, '\0');
    while (!stop_) {
        std::map<SOCKET, LinkPtr> links;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [id, link] : peers_)
                if (link->inner != INVALID_SOCKET) links[link->inner] = link;
        }
        if (links.empty()) {
            Sleep(50);
            continue;
        }
        fd_set rd;
        FD_ZERO(&rd);
        for (auto& [s, l] : links) FD_SET(s, &rd);
        timeval tv{0, 100000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;   // a socket closed under us: rebuild
        for (auto& [s, link] : links) {
            if (!FD_ISSET(s, &rd)) continue;
            sockaddr_in from{};
            int flen = sizeof(from);
            const int n = recvfrom(s, buf.data(), static_cast<int>(buf.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) continue;
            const std::string data(buf.data(), static_cast<size_t>(n));
            std::string out;
            const Frame kind = port_to_ps4(data, out);
            Endpoint peer;
            std::vector<std::string> replies;
            {
                std::lock_guard<std::mutex> lk(mu_);
                link->port_endpoint = from_sockaddr(from);
                if (kind == Frame::Game) {
                    ++link->to_peer;
                    peer = link->endpoint;
                } else if (kind == Frame::Signaling) {
                    ++link->signaling_count;
                    replies = link->signaling.handle(out);
                }
            }
            if (kind == Frame::Game) send_outer(out, peer);
            for (const auto& r : replies) to_port(link, port_signaling_frame(r));
        }
    }
}

void Gateway::tick_main() {
    double last_stun = -1e9, last_punch = 0, last_opening = 0;
    while (!stop_) {
        Sleep(100);
        const double t = now();
        if (stun_server_ && t - last_stun >= stun_refresh) {
            last_stun = t;
            send_stun();
        }
        if (t - last_punch >= punch_interval) {
            last_punch = t;
            std::vector<Endpoint> targets;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [id, l] : peers_) {
                    if (l->heard_peer || t - l->created > punch_for || relay_target(l->endpoint)) continue;
                    if (l->endpoint.valid()) targets.push_back(l->endpoint);
                    if (l->local_endpoint.valid()) targets.push_back(l->local_endpoint);
                }
            }
            for (const auto& e : targets) udp_send(outer_, kProbe, e);
        }
        if (t - last_opening >= opening_interval) {
            last_opening = t;
            std::vector<std::pair<LinkPtr, std::vector<std::string>>> openings;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [id, l] : peers_)
                    if (!l->signaling.heard && t - l->created <= opening_for)
                        openings.emplace_back(l, l->signaling.opening());
            }
            for (auto& [l, payloads] : openings)
                for (const auto& p : payloads) to_port(l, port_signaling_frame(p));
        }
    }
}

void Gateway::send_stun() {
    const std::string ip = resolve_ipv4(stun_server_->first);
    if (ip.empty()) {
        log(fmt("p2p: cannot resolve STUN server %s", stun_server_->first.c_str()));
        return;
    }
    std::string request;
    Endpoint server{ip, stun_server_->second};
    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string token = relay_ && relay_->server.ip == ip ? relay_->token : "";
        request = stun_request(token, stun_txid_);
        stun_server_ep_ = server;
    }
    udp_send(outer_, request, server);
}

void Gateway::on_stun(const std::string& data) {
    std::optional<Endpoint> changed;
    {
        std::lock_guard<std::mutex> lk(mu_);
        StunAnswer a;
        if (stun_txid_.empty() || !stun_parse(data, stun_txid_, a)) return;
        if (a.relay && stun_server_ep_.valid()) {
            if (!relay_ || relay_->port != a.relay_port) log(fmt("p2p: relay on, our port on it is %d", a.relay_port));
            relay_ = Relay{stun_server_ep_, a.relay_token, a.relay_port};
        } else if (relay_) {
            log("p2p: relay off");
            relay_.reset();
        }
        if (!mapped_ || *mapped_ != a.mapped) {
            log("p2p: STUN sees us as " + a.mapped.str());
            mapped_ = a.mapped;
            changed = a.mapped;
        }
    }
    if (changed && on_mapped_change_) {
        try {
            on_mapped_change_(*changed);
        } catch (const std::exception& e) {
            log(std::string("p2p: mapped-change callback failed: ") + e.what());
        }
    }
}

}  // namespace bb::p2p
