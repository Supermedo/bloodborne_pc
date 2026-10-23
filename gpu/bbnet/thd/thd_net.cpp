// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: sceNet and sceNetCtl. P2P sockets share one UDP port with the PS4
// kernel's vport header as bbhost writes it ([ff][c3][src][dst] for one-byte vports), the relay of
// The Hunter's Dream ([fb]['R'|'r'] framing through its STUN port), STUN Binding with the relay
// HELLO, and hole punching. Adapted from droogie/bbhost src/hle/net.cpp and src/net/stun.cpp
// (566184a), its poll path.
#include "thd_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/libraries/libs.h"
#include "core/net/p2p_transport.h"

extern "C" void runtime_voice_net_start(void);
extern "C" void runtime_voice_receive(const std::uint8_t* data, std::size_t size, std::uint64_t key);

namespace BbThd {
namespace {

// ---- sockets, the same on Windows and POSIX ------------------------------------------------------

#ifdef _WIN32
using sockfd_t = SOCKET;
constexpr sockfd_t kBadSock = INVALID_SOCKET;
using socklen_type = int;
using ssize_type = int;
void sock_close(sockfd_t fd) { ::closesocket(fd); }
int sock_errno() {
    switch (::WSAGetLastError()) {
    case WSAEWOULDBLOCK: return EAGAIN;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEACCES: return EACCES;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAENOTSOCK: return EBADF;
    case WSAEINTR: return EINTR;
    default: return EIO;
    }
}
void sock_set_nonblock(sockfd_t fd, bool on) {
    u_long v = on ? 1 : 0;
    ::ioctlsocket(fd, FIONBIO, &v);
}
int sock_poll(sockfd_t fd, short events, int ms) {
    WSAPOLLFD p{fd, events, 0};
    return ::WSAPoll(&p, 1, ms);
}
ssize_type sock_recvfrom(sockfd_t fd, void* buf, std::size_t len, bool dontwait, sockaddr_in* sa,
                         socklen_type* sl) {
    if (!dontwait && sock_poll(fd, POLLIN, -1) <= 0) return -1;
    return ::recvfrom(fd, static_cast<char*>(buf), static_cast<int>(len), 0,
                      reinterpret_cast<sockaddr*>(sa), sl);
}
ssize_type sock_sendto(sockfd_t fd, const void* buf, std::size_t len, bool dontwait,
                       const sockaddr_in* sa) {
    for (;;) {
        const int n = ::sendto(fd, static_cast<const char*>(buf), static_cast<int>(len), 0,
                               reinterpret_cast<const sockaddr*>(sa), sizeof(*sa));
        if (n >= 0 || dontwait || ::WSAGetLastError() != WSAEWOULDBLOCK) return n;
        sock_poll(fd, POLLOUT, 100);
    }
}
void wsa_start() {
    static const bool started = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)started;
}
sockfd_t sock_udp() {
    wsa_start();
    const sockfd_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd != INVALID_SOCKET) sock_set_nonblock(fd, true); // blocking calls wait with WSAPoll
    return fd;
}
void sock_setopt_int(sockfd_t fd, int level, int name, int v) {
    ::setsockopt(fd, level, name, reinterpret_cast<const char*>(&v), sizeof(v));
}
#else
using sockfd_t = int;
constexpr sockfd_t kBadSock = -1;
using socklen_type = socklen_t;
using ssize_type = ssize_t;
void sock_close(sockfd_t fd) { ::close(fd); }
int sock_errno() { return errno == EWOULDBLOCK ? EAGAIN : errno; }
void sock_set_nonblock(sockfd_t fd, bool on) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
int sock_poll(sockfd_t fd, short events, int ms) {
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, ms);
}
ssize_type sock_recvfrom(sockfd_t fd, void* buf, std::size_t len, bool dontwait, sockaddr_in* sa,
                         socklen_type* sl) {
    return ::recvfrom(fd, buf, len, dontwait ? MSG_DONTWAIT : 0, reinterpret_cast<sockaddr*>(sa), sl);
}
ssize_type sock_sendto(sockfd_t fd, const void* buf, std::size_t len, bool dontwait,
                       const sockaddr_in* sa) {
    return ::sendto(fd, buf, len, dontwait ? MSG_DONTWAIT : 0, reinterpret_cast<const sockaddr*>(sa),
                    sizeof(*sa));
}
void wsa_start() {}
sockfd_t sock_udp() { return ::socket(AF_INET, SOCK_DGRAM, 0); }
void sock_setopt_int(sockfd_t fd, int level, int name, int v) {
    ::setsockopt(fd, level, name, &v, sizeof(v));
}
#endif
bool sock_ok(sockfd_t fd) { return fd != kBadSock; }

// Host errno as the console's (FreeBSD) numbers.
int to_freebsd(int e) {
    switch (e) {
    case 0: return 0;
    case EAGAIN: return 35;
    case EINPROGRESS: return 36;
    case EALREADY: return 37;
    case ENOTSOCK: return 38;
    case EDESTADDRREQ: return 39;
    case EMSGSIZE: return 40;
    case EPROTOTYPE: return 41;
    case ENOPROTOOPT: return 42;
    case EPROTONOSUPPORT: return 43;
    case EOPNOTSUPP: return 45;
    case EAFNOSUPPORT: return 47;
    case EADDRINUSE: return 48;
    case EADDRNOTAVAIL: return 49;
    case ENETDOWN: return 50;
    case ENETUNREACH: return 51;
    case ENETRESET: return 52;
    case ECONNABORTED: return 53;
    case ECONNRESET: return 54;
    case ENOBUFS: return 55;
    case EISCONN: return 56;
    case ENOTCONN: return 57;
    case ETIMEDOUT: return 60;
    case ECONNREFUSED: return 61;
    case EHOSTUNREACH: return 65;
    default: return e;
    }
}

constexpr int kNetErrBase = static_cast<int>(0x80410100);
thread_local int g_net_errno = 0;
int net_err(int e) { return kNetErrBase | (to_freebsd(e) & 0xff); }
int net_fail() {
    const int e = sock_errno();
    g_net_errno = to_freebsd(e);
    return net_err(e);
}
int net_again() {
    g_net_errno = 35;
    return net_err(EAGAIN);
}
int sce_err(int e) { return static_cast<int>(0x80020000u | (static_cast<unsigned>(to_freebsd(e)) & 0xffffu)); }

std::uint16_t bswap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }
std::uint32_t bswap32(std::uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00ff0000u) | ((v >> 8) & 0x0000ff00u) | (v >> 24);
}
void write_ipv4(char* dst, unsigned n, std::uint32_t net) {
    const std::uint32_t h = bswap32(net);
    std::snprintf(dst, n, "%u.%u.%u.%u", (h >> 24) & 0xffu, (h >> 16) & 0xffu, (h >> 8) & 0xffu, h & 0xffu);
}

// The epoll wait sleeps on one word every reader, control change and abort bumps.
std::mutex g_wake_mu;
std::condition_variable g_wake_cv;
std::uint32_t g_wake_word = 0;
void net_wake() {
    {
        std::lock_guard lk(g_wake_mu);
        ++g_wake_word;
    }
    g_wake_cv.notify_all();
}
std::uint32_t wake_gen() {
    std::lock_guard lk(g_wake_mu);
    return g_wake_word;
}
void wake_wait(std::uint32_t gen, std::chrono::steady_clock::time_point until) {
    std::unique_lock lk(g_wake_mu);
    g_wake_cv.wait_until(lk, until, [&] { return g_wake_word != gen; });
}

bool trace_now(int& count) { return Trace() || ++count <= 64; }

// ---- STUN -------------------------------------------------------------------------------------------

constexpr std::size_t kStunHeader = 20, kStunTxid = 16, kStunMaxRequest = kStunHeader + 16;
constexpr std::uint16_t kBindingRequest = 0x0001, kBindingResponse = 0x0101;
constexpr std::uint16_t kMappedAddress = 0x0001, kXorMappedAddress = 0x0020, kXorMappedAddressOld = 0x8020;
constexpr std::uint16_t kBbhostHello = 0x8100, kBbhostRelay = 0x8101;
constexpr std::uint8_t kRelayVersion[4] = {'b', 'b', 'r', '1'};
constexpr std::uint32_t kCookie = 0x2112A442u;

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t rd32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
void wr16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void wr32(std::uint8_t* p, std::uint32_t v) {
    wr16(p, static_cast<std::uint16_t>(v >> 16));
    wr16(p + 2, static_cast<std::uint16_t>(v));
}
std::uint32_t to_nbo(std::uint32_t host) {
    std::uint8_t b[4];
    wr32(b, host);
    std::uint32_t v;
    std::memcpy(&v, b, 4);
    return v;
}

std::size_t build_binding_request(std::uint8_t* out, std::uint8_t txid[kStunTxid], bool hello,
                                  const std::uint8_t* token) {
    static thread_local std::mt19937_64 rng(
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x5bd1e995u);
    wr32(txid, kCookie);
    for (std::size_t i = 4; i < kStunTxid; i += 4) wr32(txid + i, static_cast<std::uint32_t>(rng()));
    wr16(out, kBindingRequest);
    std::memcpy(out + 4, txid, kStunTxid);
    std::size_t n = kStunHeader;
    if (hello) {
        wr16(out + n, kBbhostHello);
        wr16(out + n + 2, 12);
        std::memcpy(out + n + 4, kRelayVersion, 4);
        if (token) std::memcpy(out + n + 8, token, kStunTokenLen);
        else std::memset(out + n + 8, 0, kStunTokenLen);
        n += 16;
    }
    wr16(out + 2, static_cast<std::uint16_t>(n - kStunHeader));
    return n;
}

bool parse_binding_response(const std::uint8_t* d, std::size_t n, const std::uint8_t txid[kStunTxid],
                            std::uint32_t* addr, std::uint16_t* port, Relay* relay) {
    if (relay) *relay = Relay{};
    if (n < kStunHeader || rd16(d) != kBindingResponse || std::memcmp(d + 4, txid, kStunTxid) != 0) return false;
    const std::size_t len = rd16(d + 2);
    if (kStunHeader + len > n) return false;
    const std::uint32_t xor_addr_key = rd32(d + 4);
    const std::uint16_t xor_port_key = rd16(d + 4);
    std::uint32_t mapped = 0, xored = 0;
    std::uint16_t mapped_port = 0, xored_port = 0;
    bool have_mapped = false, have_xored = false;
    for (std::size_t at = kStunHeader; at + 4 <= kStunHeader + len;) {
        const std::uint16_t type = rd16(d + at), alen = rd16(d + at + 2);
        const std::uint8_t* v = d + at + 4;
        if (at + 4 + alen > kStunHeader + len) break;
        if (type == kBbhostRelay && relay && alen >= 20 && std::memcmp(v, kRelayVersion, 4) == 0) {
            relay->present = true;
            std::memcpy(relay->token, v + 4, kStunTokenLen);
            relay->vport = rd16(v + 12);
            relay->observed_addr = to_nbo(rd32(v + 14));
            relay->observed_port = rd16(v + 18);
        } else if (alen >= 8 && v[1] == 0x01) {
            if (type == kMappedAddress) {
                mapped_port = rd16(v + 2);
                mapped = rd32(v + 4);
                have_mapped = true;
            } else if (type == kXorMappedAddress || type == kXorMappedAddressOld) {
                xored_port = static_cast<std::uint16_t>(rd16(v + 2) ^ xor_port_key);
                xored = rd32(v + 4) ^ xor_addr_key;
                have_xored = true;
            }
        }
        at += 4 + ((alen + 3u) & ~3u);
    }
    if (!have_mapped && !have_xored) return false;
    const std::uint32_t a = have_mapped ? mapped : xored;
    const std::uint16_t p = have_mapped ? mapped_port : xored_port;
    if (!a || !p) return false;
    if (addr) *addr = to_nbo(a);
    if (port) *port = p;
    return true;
}

// ---- state --------------------------------------------------------------------------------------------

std::mutex g_net_mu;
int g_net_next = 1;

// SceNetSockaddrIn: {len, family, port (nbo), addr (nbo), vport (nbo), zero[6]}.
struct SceSockaddrIn {
    std::uint8_t len;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t addr;
    std::uint16_t vport;
    std::uint8_t zero[6];
};
static_assert(sizeof(SceSockaddrIn) == 16);

// The PS4 kernel's header on every P2P datagram: [0xff][flags][src vport][dst vport], one-byte
// vports with flag 0x40 (Bloodborne's are 40 and 30), two-byte ones without, four more bytes when
// flag 0x20 adds a comid. What bbhost players send and read.
constexpr std::uint8_t kP2pMagic = 0xff;
constexpr std::uint8_t kP2pFlagP2p = 0x80, kP2pFlagByteVports = 0x40, kP2pFlagComid = 0x20, kP2pType = 0x03;

struct Datagram {
    std::vector<std::uint8_t> data;
    std::uint32_t addr = 0;  // nbo
    std::uint16_t port = 0;  // nbo
    std::uint16_t vport = 0; // host order, the sender's
};
struct SockQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Datagram> q;
    int sock = 0;
    std::uint16_t vport = 0;
};
// One UDP socket per P2P port, shared by every game socket bound to a vport on it.
struct UdpPort {
    sockfd_t fd = kBadSock;
    std::uint16_t port = 0; // host order
    std::mutex mu;
    std::map<std::uint16_t, std::shared_ptr<SockQueue>> by_vport;
    std::thread reader;
    std::atomic<bool> stop{false};
    bool stun_pending = false, stun_done = false;
    std::uint8_t stun_txid[kStunTxid] = {};
    std::uint32_t stun_addr = 0;
    std::uint16_t stun_port = 0;
    Relay stun_relay;
    std::condition_variable stun_cv;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> heard; // addr | port << 32
};
// A hole-punch probe: not a P2P datagram (no 0xff); the receiving port drops it.
constexpr std::uint8_t kProbe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};

// The relay: once a STUN answer carried BBHOST-RELAY, a datagram for one of the server's other
// ports leaves framed for the STUN port, [0xfb]['R'][token][port], and comes back from it framed
// [0xfb]['r'][source port]. The game sees none of it: its peers stay server:port both ways.
constexpr std::uint8_t kRelayMagic = 0xfb, kRelayToServer = 'R', kRelayToClient = 'r';
constexpr std::size_t kRelayHeader = 12;
struct RelayLink {
    std::mutex mu;
    bool on = false;
    std::uint32_t server = 0;    // nbo
    std::uint16_t stun_port = 0; // nbo
    std::uint8_t token[kStunTokenLen] = {};
    std::uint16_t vport = 0;
};
RelayLink g_relay;

bool relay_target(std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t token[kStunTokenLen],
                  sockaddr_in* stun_sa) {
    std::lock_guard lk(g_relay.mu);
    if (!g_relay.on || addr != g_relay.server || port_nbo == g_relay.stun_port) return false;
    std::memcpy(token, g_relay.token, kStunTokenLen);
    *stun_sa = sockaddr_in{};
    stun_sa->sin_family = AF_INET;
    stun_sa->sin_addr.s_addr = g_relay.server;
    stun_sa->sin_port = g_relay.stun_port;
    return true;
}
bool from_relay_server(const sockaddr_in& sa) {
    std::lock_guard lk(g_relay.mu);
    return g_relay.on && sa.sin_addr.s_addr == g_relay.server && sa.sin_port == g_relay.stun_port;
}
std::uint64_t source_key(std::uint32_t addr, std::uint16_t port_nbo) {
    return addr | (static_cast<std::uint64_t>(port_nbo) << 32);
}

std::map<std::uint16_t, std::shared_ptr<UdpPort>> g_p2p_ports;

constexpr std::uint8_t kVoiceMagic = 0xbb, kVoiceTag = 'V';
std::mutex g_voice_mu;
std::map<std::uint64_t, std::chrono::steady_clock::time_point> g_voice_peers;

void voice_peer_seen(std::uint32_t addr, std::uint16_t port_nbo) {
    std::lock_guard lk(g_voice_mu);
    g_voice_peers[source_key(addr, port_nbo)] = std::chrono::steady_clock::now();
}

std::vector<std::uint64_t> voice_active_peers() {
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(3);
    std::vector<std::uint64_t> out;
    std::lock_guard lk(g_voice_mu);
    for (auto it = g_voice_peers.begin(); it != g_voice_peers.end();) {
        if (it->second < cutoff) it = g_voice_peers.erase(it);
        else out.push_back((it++)->first);
    }
    return out;
}

struct NetSock {
    int domain = 2;
    int type = 1;
    bool nonblock = false;
    // UDP (type 2) sockets are real; SOCK_DGRAM_P2P (6) share the P2P port; stream sockets are
    // inert (the game's HTTP is sceHttp).
    sockfd_t fd = kBadSock;
    bool p2p = false;
    std::uint16_t vport = 0;
    std::shared_ptr<UdpPort> port;
    std::shared_ptr<SockQueue> queue;
};
struct NetEpoll {
    std::mutex mu;
    bool aborting = false;
    std::unordered_map<int, std::uint32_t> watch;
    std::unordered_map<int, std::uint64_t> data;
};
std::unordered_map<int, NetSock> g_socks;
std::unordered_map<int, std::unique_ptr<NetEpoll>> g_epolls;
std::unordered_map<int, std::pair<void*, void*>> g_ctl_cbs;
int g_ctl_next = 1;

std::size_t p2p_header(const std::uint8_t* d, std::size_t n, std::uint16_t* src, std::uint16_t* dst) {
    if (n < 4 || d[0] != kP2pMagic) return 0;
    const std::uint8_t fl = d[1];
    std::size_t at = 2;
    if (fl & kP2pFlagComid) at += 4;
    if (fl & kP2pFlagByteVports) {
        if (at + 2 > n) return 0;
        *src = d[at];
        *dst = d[at + 1];
        return at + 2;
    }
    if (at + 4 > n) return 0;
    *src = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
    *dst = static_cast<std::uint16_t>((d[at + 2] << 8) | d[at + 3]);
    return at + 4;
}

void p2p_reader(std::shared_ptr<UdpPort> port) {
    std::uint8_t buf[2048];
    int logs = 0;
    while (!port->stop.load(std::memory_order_relaxed)) {
        if (sock_poll(port->fd, POLLIN, 200) <= 0) continue;
        sockaddr_in sa{};
        socklen_type sl = sizeof(sa);
        ssize_type n = sock_recvfrom(port->fd, buf, sizeof(buf), true, &sa, &sl);
        if (n < 0) continue;
        if (n >= 4 && buf[0] == kRelayMagic && buf[1] == kRelayToClient && from_relay_server(sa)) {
            // A relay delivery: the owner of that relay port's datagram, known as server:port.
            const std::uint16_t from_vport = static_cast<std::uint16_t>((buf[2] << 8) | buf[3]);
            std::memmove(buf, buf + 4, static_cast<std::size_t>(n - 4));
            n -= 4;
            sa.sin_port = bswap16(from_vport);
        }
        if (n >= 3 && buf[0] == kVoiceMagic && buf[1] == kVoiceTag) {
            runtime_voice_receive(buf + 2, static_cast<std::size_t>(n - 2), source_key(sa.sin_addr.s_addr, sa.sin_port));
            continue;
        }
        std::uint16_t src = 0, dst = 0;
        const std::size_t hdr = p2p_header(buf, static_cast<std::size_t>(n), &src, &dst);
        if (!hdr) {
            // A peer's hole-punch probe or the STUN server's answer: neither is the game's.
            if (n >= 8 && std::memcmp(buf, kProbe, 5) == 0) {
                std::lock_guard lk(port->mu);
                port->heard[source_key(sa.sin_addr.s_addr, sa.sin_port)] = std::chrono::steady_clock::now();
                continue;
            }
            if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01) {
                std::lock_guard lk(port->mu);
                std::uint32_t a = 0;
                std::uint16_t p = 0;
                if (port->stun_pending &&
                    parse_binding_response(buf, static_cast<std::size_t>(n), port->stun_txid, &a, &p,
                                           &port->stun_relay)) {
                    port->stun_addr = a;
                    port->stun_port = p;
                    port->stun_done = true;
                    port->stun_pending = false;
                    port->stun_cv.notify_all();
                    continue;
                }
            }
        }
        std::shared_ptr<SockQueue> q;
        bool forwarded = false;
        {
            std::lock_guard lk(port->mu);
            port->heard[source_key(sa.sin_addr.s_addr, sa.sin_port)] = std::chrono::steady_clock::now();
            auto it = hdr ? port->by_vport.find(dst) : port->by_vport.end();
            if (it != port->by_vport.end()) {
                q = it->second;
            } else if (!port->by_vport.empty()) {
                // No socket on that vport (or no header): the first bound vport takes it.
                q = port->by_vport.begin()->second;
                forwarded = true;
            }
        }
        if (!q) continue;
        Datagram dg;
        dg.data.assign(buf + hdr, buf + n);
        dg.addr = sa.sin_addr.s_addr;
        dg.port = sa.sin_port;
        dg.vport = src;
        if (trace_now(logs)) {
            char ip[32];
            write_ipv4(ip, sizeof(ip), sa.sin_addr.s_addr);
            Log("net: recv %d <- %s:%u vport %u->%u: %zu bytes%s", q->sock, ip, bswap16(sa.sin_port), src,
                dst, dg.data.size(), hdr ? (forwarded ? " (forwarded)" : "") : " (no p2p header)");
        }
        {
            std::lock_guard lk(q->mu);
            q->q.push_back(std::move(dg));
        }
        q->cv.notify_all();
        net_wake();
    }
}

// Under g_net_mu: the P2P UDP port, opened on first use (the game's first P2P bind or STUN).
std::shared_ptr<UdpPort> p2p_port_open(std::uint16_t want) {
    if (auto it = g_p2p_ports.find(want); it != g_p2p_ports.end()) return it->second;
    auto port = std::make_shared<UdpPort>();
    // Ours alone: with another program on the port, the next free one, which the server is told.
    for (int i = 0; i < 16; ++i) {
        const auto at = static_cast<std::uint16_t>(want + i);
        port->fd = sock_udp();
        if (!sock_ok(port->fd)) return nullptr;
#ifdef _WIN32
        sock_setopt_int(port->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#endif
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = bswap16(at);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (::bind(port->fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
            port->port = at;
            break;
        }
        const int e = sock_errno();
        sock_close(port->fd);
        port->fd = kBadSock;
        if (e != EADDRINUSE && e != EACCES) {
            Log("net: P2P port %u: bind failed (%d)", at, e);
            return nullptr;
        }
        Log("net: P2P port %u is in use by another program%s", at, i + 1 < 16 ? "; trying the next" : "");
    }
    if (!sock_ok(port->fd)) return nullptr;
    if (port->port != want) SetP2pPortBound(port->port);
    Log("net: P2P port %u open", port->port);
    port->reader = std::thread(p2p_reader, port);
    g_p2p_ports[port->port] = port;
    runtime_voice_net_start();
    return port;
}

// Under g_net_mu: a P2P socket leaves its port; the port closes with its last socket.
void p2p_detach(NetSock& sock) {
    if (!sock.port) return;
    std::shared_ptr<UdpPort> port = sock.port;
    bool empty = false;
    {
        std::lock_guard lk(port->mu);
        auto it = port->by_vport.find(sock.vport);
        if (it != port->by_vport.end() && it->second == sock.queue) port->by_vport.erase(it);
        empty = port->by_vport.empty();
    }
    sock.port.reset();
    if (empty) {
        port->stop.store(true, std::memory_order_relaxed);
        if (port->reader.joinable()) port->reader.join();
        if (sock_ok(port->fd)) sock_close(port->fd);
        port->fd = kBadSock;
        g_p2p_ports.erase(port->port);
    }
}

bool sock_state(int s, NetSock* out) {
    std::lock_guard lk(g_net_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return false;
    *out = it->second;
    return true;
}

int dgram_recv(int s, void* buf, std::uint64_t len, int flags, SceSockaddrIn* from) {
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(EBADF);
    const bool dontwait = sock.nonblock || (flags & 0x80); // SCE_NET_MSG_DONTWAIT
    if (sock.p2p) {
        if (!sock.queue) return net_again();
        SockQueue& q = *sock.queue;
        Datagram dg;
        {
            std::unique_lock lk(q.mu);
            if (q.q.empty()) {
                if (dontwait) return net_again();
                q.cv.wait(lk, [&] { return !q.q.empty(); });
            }
            dg = std::move(q.q.front());
            q.q.pop_front();
        }
        const std::size_t take = std::min<std::size_t>(dg.data.size(), static_cast<std::size_t>(len));
        if (buf && take) std::memcpy(buf, dg.data.data(), take);
        if (from) {
            std::memset(from, 0, sizeof(*from));
            from->len = 16;
            from->family = 2;
            from->port = dg.port;
            from->addr = dg.addr;
            from->vport = bswap16(dg.vport);
        }
        return static_cast<int>(take);
    }
    if (!sock_ok(sock.fd)) return dontwait ? net_again() : 0;
    sockaddr_in sa{};
    socklen_type sl = sizeof(sa);
    const ssize_type n = sock_recvfrom(sock.fd, buf, static_cast<std::size_t>(len), dontwait, &sa, &sl);
    if (n < 0) return sock_errno() == EAGAIN ? net_again() : net_fail();
    if (from) {
        std::memset(from, 0, sizeof(*from));
        from->len = 16;
        from->family = 2;
        from->port = sa.sin_port;
        from->addr = sa.sin_addr.s_addr;
    }
    return static_cast<int>(n);
}

int dgram_send(int s, const void* buf, std::uint64_t len, int flags, const SceSockaddrIn* to) {
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(EBADF);
    const sockfd_t fd = sock.port ? sock.port->fd : sock.fd;
    if (!sock_ok(fd)) return static_cast<int>(len);
    if (!to) return net_err(EINVAL);
    std::uint8_t pkt[2048];
    std::size_t hdr = 0;
    const std::uint16_t dst_vport = bswap16(to->vport);
    if (sock.p2p) {
        pkt[0] = kP2pMagic;
        if (sock.vport < 256 && dst_vport < 256) {
            pkt[1] = kP2pFlagP2p | kP2pFlagByteVports | kP2pType;
            pkt[2] = static_cast<std::uint8_t>(sock.vport);
            pkt[3] = static_cast<std::uint8_t>(dst_vport);
            hdr = 4;
        } else {
            pkt[1] = kP2pFlagP2p | kP2pType;
            pkt[2] = static_cast<std::uint8_t>(sock.vport >> 8);
            pkt[3] = static_cast<std::uint8_t>(sock.vport);
            pkt[4] = static_cast<std::uint8_t>(dst_vport >> 8);
            pkt[5] = static_cast<std::uint8_t>(dst_vport);
            hdr = 6;
        }
    }
    if (hdr + len > sizeof(pkt)) return net_err(EMSGSIZE);
    if (buf && len) std::memcpy(pkt + hdr, buf, static_cast<std::size_t>(len));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = to->port;
    sa.sin_addr.s_addr = to->addr;
    const bool dontwait = sock.nonblock || (flags & 0x80);
    ssize_type n;
    std::uint8_t token[kStunTokenLen];
    sockaddr_in stun_sa{};
    if (sock.p2p && relay_target(to->addr, to->port, token, &stun_sa)) {
        std::uint8_t frame[sizeof(pkt) + kRelayHeader];
        frame[0] = kRelayMagic;
        frame[1] = kRelayToServer;
        std::memcpy(frame + 2, token, kStunTokenLen);
        std::memcpy(frame + 10, &to->port, 2); // network order is the frame's big-endian
        std::memcpy(frame + kRelayHeader, pkt, hdr + static_cast<std::size_t>(len));
        n = sock_sendto(fd, frame, kRelayHeader + hdr + static_cast<std::size_t>(len), dontwait, &stun_sa);
        if (n >= 0) n -= static_cast<ssize_type>(kRelayHeader);
    } else {
        n = sock_sendto(fd, pkt, hdr + static_cast<std::size_t>(len), dontwait, &sa);
    }
    static int logs = 0;
    if (trace_now(logs)) {
        char ip[32];
        write_ipv4(ip, sizeof(ip), to->addr);
        Log("net: send %d -> %s:%u vport %u->%u: %llu bytes -> %lld", s, ip, bswap16(to->port), sock.vport,
            dst_vport, static_cast<unsigned long long>(len), static_cast<long long>(n));
    }
    if (n < 0) return sock_errno() == EAGAIN ? net_again() : net_fail();
    if (sock.p2p) voice_peer_seen(to->addr, to->port);
    return static_cast<int>(len);
}

// ---- the imports -----------------------------------------------------------------------------------

s32 PS4_SYSV_ABI net_init() {
    wsa_start();
    Log("sceNetInit");
    return 0;
}
s32 PS4_SYSV_ABI net_term() { return 0; }
int* PS4_SYSV_ABI net_errno_loc() { return &g_net_errno; }
s32 PS4_SYSV_ABI net_pool_create(const char*, int, int) {
    std::lock_guard lk(g_net_mu);
    return g_net_next++;
}
s32 PS4_SYSV_ABI net_pool_destroy(int) { return 0; }

s32 PS4_SYSV_ABI net_socket(const char* name, int domain, int type, int) {
    std::lock_guard lk(g_net_mu);
    const int id = g_net_next++;
    NetSock sock;
    sock.domain = domain;
    sock.type = type;
    if (domain == 2 && type == 2) {
        sock.fd = sock_udp();
    } else if (domain == 2 && type == 6) {
        sock.p2p = true;
        sock.queue = std::make_shared<SockQueue>();
        sock.queue->sock = id;
    }
    g_socks[id] = sock;
    Log("sceNetSocket %s domain=%d type=%d -> %d%s", name ? name : "", domain, type, id,
        sock.p2p ? " (udp p2p)" : (sock_ok(sock.fd) ? " (udp)" : ""));
    return id;
}
s32 PS4_SYSV_ABI net_socket_close(int s) {
    std::lock_guard lk(g_net_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return net_err(EBADF);
    if (sock_ok(it->second.fd)) sock_close(it->second.fd);
    p2p_detach(it->second);
    for (auto& [eid, ep] : g_epolls) {
        std::lock_guard elk(ep->mu);
        ep->watch.erase(s);
        ep->data.erase(s);
    }
    g_socks.erase(it);
    return 0;
}
s32 PS4_SYSV_ABI net_socket_abort(int s, int) {
    std::lock_guard lk(g_net_mu);
    return g_socks.count(s) ? 0 : net_err(EBADF);
}
s32 PS4_SYSV_ABI net_bind(int s, const void* addr, int len) {
    std::lock_guard lk(g_net_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return net_err(EBADF);
    NetSock& sock = it->second;
    SceSockaddrIn in{};
    if (addr && len >= static_cast<int>(sizeof(in))) std::memcpy(&in, addr, sizeof(in));
    if (sock.p2p) {
        // The P2P UDP port (BB_P2P_PORT); the vport is the game's.
        std::shared_ptr<UdpPort> port = p2p_port_open(static_cast<std::uint16_t>(P2pPort()));
        if (!port) return net_fail();
        p2p_detach(sock);
        sock.vport = bswap16(in.vport);
        sock.queue->vport = sock.vport;
        sock.port = port;
        {
            std::lock_guard plk(port->mu);
            port->by_vport[sock.vport] = sock.queue;
        }
        Log("sceNetBind %d vport %u -> udp %u (p2p)", s, sock.vport, port->port);
        return 0;
    }
    if (!sock_ok(sock.fd)) return 0;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = in.port;
    sa.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock.fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) return net_fail();
    return 0;
}
s32 PS4_SYSV_ABI net_listen(int s, int) {
    std::lock_guard lk(g_net_mu);
    return g_socks.count(s) ? 0 : net_err(EBADF);
}
s32 PS4_SYSV_ABI net_accept(int s, void* addr, unsigned* addrlen) {
    std::lock_guard lk(g_net_mu);
    if (!g_socks.count(s)) return net_err(EBADF);
    if (addr && addrlen && *addrlen >= 16) {
        std::uint8_t sa[16]{};
        sa[0] = 16;
        sa[1] = 2;
        std::memcpy(addr, sa, 16);
        *addrlen = 16;
    }
    const int id = g_net_next++;
    g_socks[id] = NetSock{};
    return id;
}
s32 PS4_SYSV_ABI net_connect(int s, const void*, int) {
    std::lock_guard lk(g_net_mu);
    return g_socks.count(s) ? 0 : net_err(EBADF);
}
s32 PS4_SYSV_ABI net_shutdown(int s, int) {
    std::lock_guard lk(g_net_mu);
    return g_socks.count(s) ? 0 : net_err(EBADF);
}
s32 PS4_SYSV_ABI net_setsockopt(int s, int level, int name, const void* val, int len) {
    std::lock_guard lk(g_net_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return net_err(EBADF);
    NetSock& sock = it->second;
    int v = 0;
    if (val && len >= 4) std::memcpy(&v, val, 4);
    if (name == 0x1200) { // SCE_NET_SO_NBIO
        sock.nonblock = v != 0;
#ifndef _WIN32
        if (sock_ok(sock.fd)) sock_set_nonblock(sock.fd, v != 0);
#endif
    } else if (sock_ok(sock.fd) && level == 0xffff && (name == 0x1001 || name == 0x1002)) {
        sock_setopt_int(sock.fd, SOL_SOCKET, name == 0x1001 ? SO_SNDBUF : SO_RCVBUF, v);
    }
    return 0;
}
s32 PS4_SYSV_ABI net_getsockopt(int s, int, int, void* val, unsigned* len) {
    std::lock_guard lk(g_net_mu);
    if (!g_socks.count(s)) return net_err(EBADF);
    if (val && len && *len >= 4) {
        std::memset(val, 0, 4);
        *len = 4;
    }
    return 0;
}
s32 PS4_SYSV_ABI net_getsockname(int s, void* addr, unsigned* addrlen) {
    std::lock_guard lk(g_net_mu);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) return net_err(EBADF);
    if (addr && addrlen && *addrlen >= 16) {
        SceSockaddrIn in{};
        in.len = 16;
        in.family = 2;
        const sockfd_t fd = it->second.port ? it->second.port->fd : it->second.fd;
        if (sock_ok(fd)) {
            sockaddr_in sa{};
            socklen_type sl = sizeof(sa);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
                in.port = sa.sin_port;
                in.addr = sa.sin_addr.s_addr;
            }
            in.vport = bswap16(it->second.vport);
        }
        std::memcpy(addr, &in, 16);
        *addrlen = 16;
    }
    return 0;
}
s32 PS4_SYSV_ABI net_recv(int s, void* buf, u64 len, int flags) { return dgram_recv(s, buf, len, flags, nullptr); }
s32 PS4_SYSV_ABI net_send(int s, const void* buf, u64 len, int) {
    NetSock sock;
    if (!sock_state(s, &sock)) return net_err(EBADF);
    return static_cast<int>(len); // stream sockets are inert
}
s32 PS4_SYSV_ABI net_recvfrom(int s, void* buf, u64 len, int flags, void* from, unsigned* fromlen) {
    SceSockaddrIn in{};
    const int r = dgram_recv(s, buf, len, flags, &in);
    if (r >= 0 && from && fromlen && *fromlen >= 16) {
        std::memcpy(from, &in, 16);
        *fromlen = 16;
    }
    return r;
}
s32 PS4_SYSV_ABI net_sendto(int s, const void* buf, u64 len, int flags, const void* to, int tolen) {
    SceSockaddrIn in{};
    if (to && tolen >= 16) std::memcpy(&in, to, 16);
    return dgram_send(s, buf, len, flags, (to && tolen >= 16) ? &in : nullptr);
}
u16 PS4_SYSV_ABI net_htons(u16 v) { return bswap16(v); }
u32 PS4_SYSV_ABI net_htonl(u32 v) { return bswap32(v); }
s32 PS4_SYSV_ABI net_inet_pton(int af, const char* src, void* dst) {
    if (af != 2 || !dst) return net_err(EAFNOSUPPORT);
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (!src || std::sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 ||
        c > 255 || d > 255) {
        return 0;
    }
    const std::uint32_t net = bswap32((a << 24) | (b << 16) | (c << 8) | d);
    std::memcpy(dst, &net, 4);
    return 1;
}
const char* PS4_SYSV_ABI net_inet_ntop(int af, const void* src, char* dst, unsigned size) {
    if (af != 2 || !src || !dst || size < 8) return nullptr;
    std::uint32_t net = 0;
    std::memcpy(&net, src, 4);
    write_ipv4(dst, size, net);
    return dst;
}

s32 PS4_SYSV_ABI net_epoll_create(const char*, int) {
    std::lock_guard lk(g_net_mu);
    const int id = g_net_next++;
    g_epolls[id] = std::make_unique<NetEpoll>();
    return id;
}
s32 PS4_SYSV_ABI net_epoll_destroy(int id) {
    {
        std::lock_guard lk(g_net_mu);
        auto it = g_epolls.find(id);
        if (it == g_epolls.end()) return net_err(EBADF);
        g_epolls.erase(it);
    }
    net_wake();
    return 0;
}
// SceNetEpollEvent: {u32 events, u32 pad, u64 ident, u64 data}.
struct SceEpollEvent {
    std::uint32_t events;
    std::uint32_t pad;
    std::uint64_t ident;
    std::uint64_t data;
};
s32 PS4_SYSV_ABI net_epoll_control(int id, int op, int sock, void* event) {
    {
        std::lock_guard lk(g_net_mu);
        auto it = g_epolls.find(id);
        if (it == g_epolls.end()) return net_err(EBADF);
        NetEpoll& ep = *it->second;
        SceEpollEvent sev{};
        if (event) std::memcpy(&sev, event, sizeof(sev));
        std::lock_guard elk(ep.mu);
        if (op == 3) {
            ep.watch.erase(sock);
            ep.data.erase(sock);
        } else {
            ep.watch[sock] = sev.events;
            ep.data[sock] = sev.data;
        }
    }
    net_wake();
    return 0;
}
s32 PS4_SYSV_ABI net_epoll_wait(int id, void* events, int maxevents, int timeout) {
    // timeout in microseconds, -1 forever.
    const auto deadline = timeout < 0 ? std::chrono::steady_clock::time_point::max()
                                      : std::chrono::steady_clock::now() + std::chrono::microseconds(timeout);
    for (;;) {
        const std::uint32_t gen = wake_gen(); // before the look: a wake after it ends the sleep
        int out = 0;
        {
            std::lock_guard lk(g_net_mu);
            auto it = g_epolls.find(id);
            if (it == g_epolls.end()) return net_err(EBADF);
            NetEpoll& ep = *it->second;
            std::lock_guard elk(ep.mu);
            if (ep.aborting) {
                ep.aborting = false;
                return 0;
            }
            for (const auto& [sock, want] : ep.watch) {
                auto sit = g_socks.find(sock);
                if (sit == g_socks.end()) continue;
                const NetSock& ns = sit->second;
                std::uint32_t ready = 0;
                if (ns.p2p && ns.queue) {
                    std::lock_guard qlk(ns.queue->mu);
                    if ((want & 0x1) && !ns.queue->q.empty()) ready |= 0x1;
                    if (want & 0x2) ready |= 0x2; // a datagram send never waits
                } else if (sock_ok(ns.fd)) {
                    short ev = 0;
                    if (want & 0x1) ev |= POLLIN;
                    if (want & 0x2) ev |= POLLOUT;
                    if (ev && sock_poll(ns.fd, ev, 0) > 0) ready = want & 0x3;
                }
                if (!ready) continue;
                SceEpollEvent sev{};
                sev.events = ready;
                sev.ident = static_cast<std::uint64_t>(sock);
                const auto d = ep.data.find(sock);
                sev.data = d == ep.data.end() ? 0 : d->second;
                if (events && out < maxevents) {
                    std::memcpy(static_cast<std::uint8_t*>(events) + static_cast<std::size_t>(out) * sizeof(sev),
                                &sev, sizeof(sev));
                    ++out;
                }
            }
        }
        if (out) return out;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        // UDP sockets are polled, so the wait is ten milliseconds at a time.
        const auto slice = now + std::chrono::milliseconds(10);
        wake_wait(gen, deadline < slice ? deadline : slice);
    }
}
s32 PS4_SYSV_ABI net_epoll_abort(int id, int) {
    {
        std::lock_guard lk(g_net_mu);
        auto it = g_epolls.find(id);
        if (it == g_epolls.end()) return net_err(EBADF);
        std::lock_guard elk(it->second->mu);
        it->second->aborting = true;
    }
    net_wake();
    return 0;
}

s32 PS4_SYSV_ABI net_resolver_create(const char*, int, int) {
    std::lock_guard lk(g_net_mu);
    return g_net_next++;
}
s32 PS4_SYSV_ABI net_resolver_destroy(int) { return 0; }
s32 PS4_SYSV_ABI net_resolver_ntoa(int, const char*, void* addr, int, int, int) {
    if (addr) {
        const std::uint32_t loopback = bswap32(0x7f000001u);
        std::memcpy(addr, &loopback, 4);
    }
    return 0;
}
s32 PS4_SYSV_ABI net_resolver_aton(int, const void*, char* hostname, int len, int, int, int) {
    if (hostname && len > 0) std::snprintf(hostname, static_cast<std::size_t>(len), "localhost");
    return 0;
}

s32 PS4_SYSV_ABI ctl_get_state(int* state) {
    if (!state) return sce_err(EINVAL);
    *state = 3; // IPOBTAINED
    return 0;
}
s32 PS4_SYSV_ABI ctl_get_info(int code, void* info) {
    if (!info) return sce_err(EINVAL);
    std::memset(info, 0, 16);
    switch (code) {
    case 2: {
        static const std::uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        std::memcpy(info, mac, 6);
        break;
    }
    case 4: {
        const std::uint32_t mtu = 1500;
        std::memcpy(info, &mtu, 4);
        break;
    }
    case 5:
    case 12: {
        const std::uint32_t one = 1;
        std::memcpy(info, &one, 4);
        break;
    }
    case 14:
    case 16: std::snprintf(static_cast<char*>(info), 16, "127.0.0.1"); break;
    case 15: std::snprintf(static_cast<char*>(info), 16, "255.0.0.0"); break;
    case 17:
    case 18: std::snprintf(static_cast<char*>(info), 16, "0.0.0.0"); break;
    default: break;
    }
    return 0;
}
s32 PS4_SYSV_ABI ctl_register(void* fn, void* arg, int* cid) {
    std::lock_guard lk(g_net_mu);
    const int id = g_ctl_next++;
    g_ctl_cbs[id] = {fn, arg};
    if (cid) *cid = id;
    return 0;
}
s32 PS4_SYSV_ABI ctl_unregister(int cid) {
    std::lock_guard lk(g_net_mu);
    g_ctl_cbs.erase(cid);
    return 0;
}
s32 PS4_SYSV_ABI ctl_check() { return 0; }
s32 PS4_SYSV_ABI ctl_nat(void* info) {
    if (info) {
        const int stun = 1, nat = 1;
        const std::uint32_t addr = bswap32(0x7f000001u);
        auto* p = static_cast<std::uint8_t*>(info);
        std::memcpy(p, &stun, 4);
        std::memcpy(p + 4, &nat, 4);
        std::memcpy(p + 8, &addr, 4);
    }
    return 0;
}

} // namespace

// ---- STUN, relay and punch for the session layer ------------------------------------------------------

bool P2pStun(const char* host, std::uint16_t sport, int timeout_ms, std::uint32_t* mapped_addr,
             std::uint16_t* mapped_port, Relay* relay) {
    wsa_start();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        Log("stun: cannot resolve %s", host);
        return false;
    }
    sockaddr_in sa{};
    std::memcpy(&sa, res->ai_addr, sizeof(sa));
    sa.sin_port = bswap16(sport);
    freeaddrinfo(res);
    std::shared_ptr<UdpPort> port;
    {
        std::lock_guard lk(g_net_mu);
        port = p2p_port_open(static_cast<std::uint16_t>(P2pPort()));
    }
    if (!port) return false;
    std::uint8_t token[kStunTokenLen] = {};
    bool have_token = false;
    if (relay) {
        std::lock_guard rl(g_relay.mu);
        if (g_relay.on && g_relay.server == sa.sin_addr.s_addr && g_relay.stun_port == sa.sin_port) {
            std::memcpy(token, g_relay.token, sizeof(token));
            have_token = true;
        }
    }
    std::uint8_t req[kStunMaxRequest];
    std::unique_lock lk(port->mu);
    if (port->stun_pending) return false; // one at a time
    const std::size_t req_len =
        build_binding_request(req, port->stun_txid, relay != nullptr, have_token ? token : nullptr);
    port->stun_pending = true;
    port->stun_done = false;
    lk.unlock();
    (void)sock_sendto(port->fd, req, req_len, true, &sa);
    lk.lock();
    port->stun_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return port->stun_done; });
    const bool ok = port->stun_done;
    port->stun_pending = false;
    if (!ok) return false;
    if (mapped_addr) *mapped_addr = port->stun_addr;
    if (mapped_port) *mapped_port = port->stun_port;
    if (relay) {
        *relay = port->stun_relay;
        std::lock_guard rl(g_relay.mu);
        const bool was = g_relay.on;
        const std::uint16_t was_vport = g_relay.vport;
        if (relay->present) {
            g_relay.on = true;
            g_relay.server = sa.sin_addr.s_addr;
            g_relay.stun_port = sa.sin_port;
            std::memcpy(g_relay.token, relay->token, sizeof(g_relay.token));
            g_relay.vport = relay->vport;
        } else {
            g_relay.on = false; // a server without the relay: datagrams go as addressed
        }
        if (g_relay.on != was || g_relay.vport != was_vport) {
            if (g_relay.on) Log("net: relay on: %s:%u is our port %u", host, sport, g_relay.vport);
            else Log("net: relay off (%s:%u offers none)", host, sport);
        }
    }
    return true;
}

void P2pPunch(const char* label, std::uint32_t addr, std::uint16_t port_host, std::uint32_t local_addr,
              std::uint16_t local_port) {
    std::shared_ptr<UdpPort> port;
    {
        std::lock_guard lk(g_net_mu);
        auto it = g_p2p_ports.find(static_cast<std::uint16_t>(P2pPort()));
        if (it != g_p2p_ports.end()) port = it->second;
    }
    if (!port || !addr || !port_host) return;
    {
        std::uint8_t token[kStunTokenLen];
        sockaddr_in stun_sa{};
        if (relay_target(addr, bswap16(port_host), token, &stun_sa)) return; // the relay needs no hole
    }
    std::vector<sockaddr_in> targets;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = bswap16(port_host);
    targets.push_back(a);
    if (local_addr && local_port && (local_addr != addr || local_port != port_host)) {
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_addr.s_addr = local_addr;
        b.sin_port = bswap16(local_port);
        targets.push_back(b);
    }
    const std::string name = label ? label : "peer";
    std::thread([port, targets, name] {
        const auto start = std::chrono::steady_clock::now();
        int sent = 0;
        bool heard = false;
        for (int i = 0; i < 40 && !port->stop.load(std::memory_order_relaxed); ++i) {
            for (const sockaddr_in& t : targets) {
                if (sock_sendto(port->fd, kProbe, sizeof(kProbe), true, &t) >= 0) ++sent;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard lk(port->mu);
            for (const sockaddr_in& t : targets) {
                auto it = port->heard.find(source_key(t.sin_addr.s_addr, t.sin_port));
                if (it != port->heard.end() && it->second >= start) heard = true;
            }
            if (heard) break;
        }
        char ip[32];
        write_ipv4(ip, sizeof(ip), targets[0].sin_addr.s_addr);
        Log("net: punch %s %s:%u: %d probes, %s", name.c_str(), ip, bswap16(targets[0].sin_port), sent,
            heard ? "peer heard" : "nothing heard");
    }).detach();
}

std::string DetectLanAddress(const std::string& host) {
    wsa_start();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    sockaddr_in to{};
    std::memcpy(&to, res->ai_addr, sizeof(to));
    to.sin_port = bswap16(3478);
    freeaddrinfo(res);
    const sockfd_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!sock_ok(fd)) return "";
    std::string out;
    sockaddr_in local{};
    socklen_type len = sizeof(local);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0 && local.sin_addr.s_addr) {
        char ip[32];
        write_ipv4(ip, sizeof(ip), local.sin_addr.s_addr);
        out = ip;
    }
    sock_close(fd);
    return out;
}

void RegisterNet(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Nlev7Lg8k3A", "libSceNet", 1, "libSceNet", net_init);
    LIB_FUNCTION("cTGkc6-TBlI", "libSceNet", 1, "libSceNet", net_term);
    LIB_FUNCTION("HQOwnfMGipQ", "libSceNet", 1, "libSceNet", net_errno_loc);
    LIB_FUNCTION("dgJBaeJnGpo", "libSceNet", 1, "libSceNet", net_pool_create);
    LIB_FUNCTION("K7RlrTkI-mw", "libSceNet", 1, "libSceNet", net_pool_destroy);
    LIB_FUNCTION("Q4qBuN-c0ZM", "libSceNet", 1, "libSceNet", net_socket);
    LIB_FUNCTION("45ggEzakPJQ", "libSceNet", 1, "libSceNet", net_socket_close);
    LIB_FUNCTION("zJGf8xjFnQE", "libSceNet", 1, "libSceNet", net_socket_abort);
    LIB_FUNCTION("bErx49PgxyY", "libSceNet", 1, "libSceNet", net_bind);
    LIB_FUNCTION("kOj1HiAGE54", "libSceNet", 1, "libSceNet", net_listen);
    LIB_FUNCTION("PIWqhn9oSxc", "libSceNet", 1, "libSceNet", net_accept);
    LIB_FUNCTION("OXXX4mUk3uk", "libSceNet", 1, "libSceNet", net_connect);
    LIB_FUNCTION("TSM6whtekok", "libSceNet", 1, "libSceNet", net_shutdown);
    LIB_FUNCTION("2mKX2Spso7I", "libSceNet", 1, "libSceNet", net_setsockopt);
    LIB_FUNCTION("xphrZusl78E", "libSceNet", 1, "libSceNet", net_getsockopt);
    LIB_FUNCTION("hoOAofhhRvE", "libSceNet", 1, "libSceNet", net_getsockname);
    LIB_FUNCTION("9wO9XrMsNhc", "libSceNet", 1, "libSceNet", net_recv);
    LIB_FUNCTION("beRjXBn-z+o", "libSceNet", 1, "libSceNet", net_send);
    LIB_FUNCTION("304ooNZxWDY", "libSceNet", 1, "libSceNet", net_recvfrom);
    LIB_FUNCTION("gvD1greCu0A", "libSceNet", 1, "libSceNet", net_sendto);
    LIB_FUNCTION("iWQWrwiSt8A", "libSceNet", 1, "libSceNet", net_htons);
    LIB_FUNCTION("Rbvt+5Y2iEw", "libSceNet", 1, "libSceNet", net_htons); // ntohs
    LIB_FUNCTION("9T2pDF2Ryqg", "libSceNet", 1, "libSceNet", net_htonl);
    LIB_FUNCTION("pQGpHYopAIY", "libSceNet", 1, "libSceNet", net_htonl); // ntohl
    LIB_FUNCTION("8Kcp5d-q1Uo", "libSceNet", 1, "libSceNet", net_inet_pton);
    LIB_FUNCTION("9vA2aW+CHuA", "libSceNet", 1, "libSceNet", net_inet_ntop);
    LIB_FUNCTION("SF47kB2MNTo", "libSceNet", 1, "libSceNet", net_epoll_create);
    LIB_FUNCTION("Inp1lfL+Jdw", "libSceNet", 1, "libSceNet", net_epoll_destroy);
    LIB_FUNCTION("ZVw46bsasAk", "libSceNet", 1, "libSceNet", net_epoll_control);
    LIB_FUNCTION("drjIbDbA7UQ", "libSceNet", 1, "libSceNet", net_epoll_wait);
    LIB_FUNCTION("w21YgGGNtBk", "libSceNet", 1, "libSceNet", net_epoll_abort);
    LIB_FUNCTION("C4UgDHHPvdw", "libSceNet", 1, "libSceNet", net_resolver_create);
    LIB_FUNCTION("kJlYH5uMAWI", "libSceNet", 1, "libSceNet", net_resolver_destroy);
    LIB_FUNCTION("Nd91WaWmG2w", "libSceNet", 1, "libSceNet", net_resolver_ntoa);
    LIB_FUNCTION("Apb4YDxKsRI", "libSceNet", 1, "libSceNet", net_resolver_aton);
    LIB_FUNCTION("uBPlr0lbuiI", "libSceNetCtl", 1, "libSceNetCtl", ctl_get_state);
    LIB_FUNCTION("obuxdTiwkF8", "libSceNetCtl", 1, "libSceNetCtl", ctl_get_info);
    LIB_FUNCTION("UJ+Z7Q+4ck0", "libSceNetCtl", 1, "libSceNetCtl", ctl_register);
    LIB_FUNCTION("Rqm2OnZMCz0", "libSceNetCtl", 1, "libSceNetCtl", ctl_unregister);
    LIB_FUNCTION("iQw3iQPhvUQ", "libSceNetCtl", 1, "libSceNetCtl", ctl_check);
    LIB_FUNCTION("JO4yuTuMoKI", "libSceNetCtl", 1, "libSceNetCtl", ctl_nat);
}

} // namespace BbThd

extern "C" int bbgpu_voice_peers(void) {
    return static_cast<int>(BbThd::voice_active_peers().size()) + Core::Net::P2P::VoicePeerCount();
}

extern "C" int bbgpu_voice_send(const std::uint8_t* data, std::size_t size) {
    using namespace BbThd;
    if (!data || !size || size + 2 > 1200) return 0;
    int sent = Core::Net::P2P::SendVoice({data, size});
    sockfd_t fd = kBadSock;
    {
        std::lock_guard lk(g_net_mu);
        if (!g_p2p_ports.empty()) fd = g_p2p_ports.begin()->second->fd;
    }
    if (!sock_ok(fd)) return sent;
    std::uint8_t frame[kRelayHeader + 2 + 1200];
    std::uint8_t* pkt = frame + kRelayHeader;
    pkt[0] = kVoiceMagic;
    pkt[1] = kVoiceTag;
    std::memcpy(pkt + 2, data, size);
    for (const std::uint64_t key : voice_active_peers()) {
        const auto addr = static_cast<std::uint32_t>(key);
        const auto port = static_cast<std::uint16_t>(key >> 32);
        std::uint8_t token[kStunTokenLen];
        sockaddr_in sa{};
        ssize_type n;
        if (relay_target(addr, port, token, &sa)) {
            frame[0] = kRelayMagic;
            frame[1] = kRelayToServer;
            std::memcpy(frame + 2, token, kStunTokenLen);
            std::memcpy(frame + 10, &port, 2);
            n = sock_sendto(fd, frame, kRelayHeader + 2 + size, true, &sa);
        } else {
            sa.sin_family = AF_INET;
            sa.sin_port = port;
            sa.sin_addr.s_addr = addr;
            n = sock_sendto(fd, pkt, 2 + size, true, &sa);
        }
        if (n >= 0) ++sent;
    }
    return sent;
}
