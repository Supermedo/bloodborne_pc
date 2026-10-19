// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: settings, the account, the session service client, the dispatcher,
// the peer table, the event poller and the keepalive. Adapted from droogie/bbhost
// src/net/{http,account,session}.cpp and src/engine/summon_invite.cpp (566184a).
#include "httplib.h"

#include "thd.h"
#include "thd_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace BbEngine {
bool host_read_safe(const void* p, void* out, std::size_t n);
}

namespace BbThd {

// ---- log, settings ------------------------------------------------------------------------------

void Log(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("THD: %s\n", text);
}

namespace {
std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}
std::string HostOf(std::string base) {
    if (const auto s = base.find("://"); s != std::string::npos) base = base.substr(s + 3);
    if (const auto slash = base.find('/'); slash != std::string::npos) base = base.substr(0, slash);
    if (const auto colon = base.find(':'); colon != std::string::npos) base = base.substr(0, colon);
    return base;
}
} // namespace

bool Trace() {
    static const bool on = Env("BB_THD_TRACE") == "1";
    return on;
}

const Config& Cfg() {
    static const Config c = [] {
        Config c;
        c.np_base = Env("BB_THD_SERVER");
        if (c.np_base.empty()) c.np_base = "https://thehuntersdream.com:18671";
        while (!c.np_base.empty() && c.np_base.back() == '/') c.np_base.pop_back();
        c.auth_base = Env("BB_THD_AUTH");
        if (c.auth_base.empty()) c.auth_base = "https://thehuntersdream.com";
        while (!c.auth_base.empty() && c.auth_base.back() == '/') c.auth_base.pop_back();
        c.stun = Env("BB_THD_STUN");
        c.ruleset = Env("BB_THD_RULESET");
        if (c.ruleset.empty()) c.ruleset = "vanilla";
        // "BloodBorneButPc 1.2" -> "BloodBorneButPc/1.2": the server tells clients apart by it.
        c.client = Env("BB_CLIENT_VERSION");
        if (c.client.empty()) c.client = "BloodBorneButPc";
        std::replace(c.client.begin(), c.client.end(), ' ', '/');
        c.p2p_addr = Env("BB_THD_P2P_ADDR");
        const std::string port = Env("BB_P2P_PORT");
        c.p2p_port = port.empty() ? 3658 : std::atoi(port.c_str());
        if (c.p2p_port <= 0 || c.p2p_port > 65535) c.p2p_port = 3658;
        c.verify_tls = Env("BB_THD_VERIFY_TLS") != "0";
        return c;
    }();
    return c;
}

bool Enabled() {
    static const bool on = Env("BB_ONLINE") == "1" && Env("BB_NET_BACKEND") == "thd";
    return on;
}

// ---- the account ----------------------------------------------------------------------------------

namespace {
std::mutex g_account_mu;
std::string g_name, g_token;
bool g_refused = false;
// Read on first use: the game may ask for its NP state before the online start.
void LoadAccountLocked() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    g_name = Env("BB_THD_ONLINE_ID").substr(0, 16);
    g_token = Env("BB_THD_TOKEN");
}
} // namespace

std::string AccountName() {
    std::lock_guard lk(g_account_mu);
    LoadAccountLocked();
    return g_name;
}
std::string AccountToken() {
    std::lock_guard lk(g_account_mu);
    LoadAccountLocked();
    return g_token;
}
bool SignedIn() {
    std::lock_guard lk(g_account_mu);
    LoadAccountLocked();
    return !g_name.empty() && !g_token.empty();
}
bool Refused() {
    std::lock_guard lk(g_account_mu);
    return g_refused;
}
void MarkRefused(const std::string& reason) {
    std::string name;
    {
        std::lock_guard lk(g_account_mu);
        if (g_refused) return;
        g_refused = true;
        name = g_name;
    }
    Log("the server refused this PC's sign-in as %s (%s): sign in again on the launcher's Online "
        "page; until then nobody can summon or invite this player",
        name.c_str(), reason.c_str());
}
std::string OnlineId() {
    const std::string name = AccountName();
    return name.empty() ? "Player" : name;
}

void Initialize() {
    const Config& c = Cfg();
    if (SignedIn()) {
        Log("The Hunter's Dream as %s: session service %s, P2P port %d", AccountName().c_str(),
            c.np_base.c_str(), c.p2p_port);
    } else {
        Log("not signed in to The Hunter's Dream (the launcher's Online page): the game stays offline");
    }
}

// ---- JSON ---------------------------------------------------------------------------------------

std::string StrOf(const Json& o, const char* key, const std::string& dflt) {
    if (!o.is_object()) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number()) return std::to_string(static_cast<long long>(it->get<double>()));
    return dflt;
}
long long IntOf(const Json& o, const char* key, long long dflt) {
    if (!o.is_object()) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) return std::strtoll(it->get<std::string>().c_str(), nullptr, 10);
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    return dflt;
}

// ---- the session service client -----------------------------------------------------------------

namespace {
struct HttpResult {
    int status = 0;
    std::string body;
    std::string error;
};

// One keep-alive connection per thread and server: the poller asks four times a second.
HttpResult Perform(const std::string& base, const std::string& path, const std::string* post,
                   int timeout_ms) {
    thread_local std::map<std::string, std::unique_ptr<httplib::Client>> clients;
    auto& cli = clients[base];
    const auto timeout = std::chrono::milliseconds(timeout_ms);
    auto open = [&] {
        cli = std::make_unique<httplib::Client>(base);
        cli->set_keep_alive(true);
        cli->set_follow_location(false);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        cli->enable_server_certificate_verification(Cfg().verify_tls);
#endif
    };
    const bool reused = cli != nullptr;
    if (!cli) open();
    httplib::Headers headers{{"Accept", "application/json"},
                             {"User-Agent", Cfg().client},
                             {"X-BBHost-Ruleset", Cfg().ruleset}};
    if (const std::string token = AccountToken(); !token.empty()) {
        headers.emplace("X-BB-Token", token);
    }
    // Connecting and sending stay short; only the read waits as long as the request asks (a
    // long poll waits 30 s for its answer, its connection and TLS a few seconds).
    const auto short_timeout = std::min(timeout, std::chrono::milliseconds(5000));
    auto send = [&] {
        cli->set_connection_timeout(short_timeout);
        cli->set_read_timeout(timeout);
        cli->set_write_timeout(short_timeout);
        return post ? cli->Post(path, headers, *post, "application/json") : cli->Get(path, headers);
    };
    HttpResult r;
    httplib::Result res = send();
    // A kept-alive connection the server closed meanwhile fails on its first read or write
    // ("Failed to read connection"): once more on a new one, as bbhost's fresh request would.
    if (!res && reused && (res.error() == httplib::Error::Read || res.error() == httplib::Error::Write)) {
        open();
        res = send();
    }
    if (!res) {
        r.error = httplib::to_string(res.error());
        cli.reset(); // a fresh connection next time
        return r;
    }
    r.status = res->status;
    r.body = res->body;
    return r;
}

bool ParseReply(const std::string& what, const HttpResult& r, Json& out, std::string& error) {
    static std::atomic<int> logs{0};
    if (!r.error.empty()) {
        error = r.error;
    } else if (r.status < 200 || r.status >= 300) {
        error = "HTTP " + std::to_string(r.status);
        // 401 is the server refusing the sign-in, not a passing failure.
        if (r.status == 401) {
            const Json body = Json::parse(r.body, nullptr, false);
            const std::string why = StrOf(body, "Message");
            MarkRefused(why.empty() ? "HTTP 401" : why);
        }
    } else {
        out = Json::parse(r.body, nullptr, false);
        if (!out.is_discarded()) return true;
        error = "bad JSON";
    }
    if (logs.fetch_add(1) < 16) Log("np: %s -> %s", what.c_str(), error.c_str());
    return false;
}
} // namespace

std::string NpBase() {
    return Cfg().np_base;
}

bool NpPost(const std::string& path, const Json& body, Json& out, std::string& error,
            int timeout_ms) {
    const std::string text = body.dump();
    return ParseReply("POST " + path, Perform(Cfg().np_base, path, &text, timeout_ms), out, error);
}

bool NpGet(const std::string& path, Json& out, std::string& error, int timeout_ms) {
    return ParseReply("GET " + path, Perform(Cfg().np_base, path, nullptr, timeout_ms), out, error);
}

void AddGameHeaders(std::vector<std::pair<std::string, std::string>>& headers) {
    // The game's own traffic carries the token too, so the server's game sessions map to the
    // account, and the ruleset, so its messages and bloodstains stay among the same rules.
    if (const std::string token = AccountToken(); !token.empty()) {
        headers.emplace_back("X-BB-Token", token);
    }
    headers.emplace_back("X-BBHost-Ruleset", Cfg().ruleset);
    for (auto& [name, value] : headers) {
        if (name.size() == 10 && std::equal(name.begin(), name.end(), "user-agent", [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            value += " " + Cfg().client;
            break;
        }
    }
}

// ---- the dispatcher -------------------------------------------------------------------------------

namespace {
using Clock = std::chrono::steady_clock;
struct Due {
    Clock::time_point at;
    int prio;
    std::uint64_t seq;
    std::function<void()> fn;
    bool operator<(const Due& o) const {
        if (at != o.at) return at < o.at;
        if (prio != o.prio) return prio < o.prio;
        return seq < o.seq;
    }
};
std::mutex g_dmu;
std::condition_variable g_dcv;
std::deque<Due> g_due; // sorted
std::uint64_t g_seq = 0;
bool g_dispatcher = false;

void* PS4_SYSV_ABI DispatcherMain(void*) {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lk(g_dmu);
            for (;;) {
                if (!g_due.empty()) {
                    if (g_due.front().at <= Clock::now()) {
                        fn = std::move(g_due.front().fn);
                        g_due.pop_front();
                        break;
                    }
                    g_dcv.wait_until(lk, g_due.front().at);
                } else {
                    g_dcv.wait(lk);
                }
            }
        }
        try {
            fn();
        } catch (const std::exception& e) {
            Log("dispatcher: %s", e.what());
        } catch (...) {
            Log("dispatcher: unknown exception");
        }
    }
    return nullptr;
}
} // namespace

void DispatchAfter(int delay_ms, Prio prio, std::function<void()> fn) {
    std::lock_guard lk(g_dmu);
    if (!g_dispatcher) {
        // A guest thread: the game's callbacks run on it, as on the console's NP threads.
        void* thread = nullptr;
        if (runtime_thread_spawn(&thread, DispatcherMain, nullptr, 1 << 20, "THD:np") == 0) {
            g_dispatcher = true;
        } else {
            Log("dispatcher: the NP thread did not start; online callbacks will not arrive");
        }
    }
    Due d{Clock::now() + std::chrono::milliseconds(delay_ms < 0 ? 0 : delay_ms),
          static_cast<int>(prio), g_seq++, std::move(fn)};
    auto it = g_due.begin();
    while (it != g_due.end() && *it < d) ++it;
    g_due.insert(it, std::move(d));
    g_dcv.notify_all();
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

// ---- our endpoint -----------------------------------------------------------------------------------

namespace {
std::atomic<int> g_p2p_bound{0};
std::atomic<int> g_stun_rtt_us{0};
} // namespace

std::string LocalAddrText() {
    static const std::string addr = [] {
        if (!Cfg().p2p_addr.empty()) return Cfg().p2p_addr;
        const std::string lan = DetectLanAddress(HostOf(Cfg().np_base));
        return lan.empty() ? std::string("127.0.0.1") : lan;
    }();
    return addr;
}
int P2pPort() {
    const int bound = g_p2p_bound.load(std::memory_order_relaxed);
    return bound > 0 ? bound : Cfg().p2p_port;
}
void SetP2pPortBound(int port) {
    g_p2p_bound.store(port, std::memory_order_relaxed);
}
int StunRttUs() {
    return g_stun_rtt_us.load();
}

// ---- the peer table -----------------------------------------------------------------------------------

namespace {
std::mutex g_pmu;
std::map<std::uint16_t, Peer> g_peers;

std::uint32_t AddrOf(const std::string& text) {
    in_addr a{};
    if (text.empty() || inet_pton(AF_INET, text.c_str(), &a) != 1) return 0;
    return a.s_addr;
}

// A peer with a new address: probes toward it so our NAT lets its datagrams in.
void PunchIfNew(const Peer& before, const Peer& after) {
    if (after.online_id == OnlineId() || !after.addr || !after.port) return;
    if (before.addr == after.addr && before.port == after.port &&
        before.local_addr == after.local_addr && before.local_port == after.local_port) {
        return;
    }
    P2pPunch(after.online_id.c_str(), after.addr, after.port, after.local_addr, after.local_port);
}
} // namespace

void PeersClear() {
    std::lock_guard lk(g_pmu);
    g_peers.clear();
}
void PeersUpsert(const Peer& p) {
    Peer before;
    {
        std::lock_guard lk(g_pmu);
        Peer& slot = g_peers[p.member_id];
        before = slot;
        const unsigned conn = p.conn_id ? p.conn_id : slot.conn_id;
        const int sig = p.sig_status ? p.sig_status : slot.sig_status;
        slot = p;
        slot.conn_id = conn;
        slot.sig_status = sig;
    }
    PunchIfNew(before, p);
}
bool PeersGet(std::uint16_t member_id, Peer* out) {
    std::lock_guard lk(g_pmu);
    const auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return false;
    if (out) *out = it->second;
    return true;
}
bool PeersFindOnline(const std::string& id, Peer* out) {
    std::lock_guard lk(g_pmu);
    for (const auto& [m, p] : g_peers) {
        if (p.online_id == id) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}
void PeersSetConn(std::uint16_t member_id, unsigned conn_id, int sig_status) {
    std::lock_guard lk(g_pmu);
    const auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return;
    it->second.conn_id = conn_id;
    it->second.sig_status = sig_status;
}
void PeersErase(std::uint16_t member_id) {
    std::lock_guard lk(g_pmu);
    g_peers.erase(member_id);
}
std::vector<Peer> PeersAll() {
    std::lock_guard lk(g_pmu);
    std::vector<Peer> v;
    for (const auto& [m, p] : g_peers) v.push_back(p);
    return v;
}
int PeersFromMembers(const Json& members) {
    if (!members.is_array()) return 0;
    int n = 0;
    for (const Json& m : members) {
        Peer p;
        p.member_id = static_cast<std::uint16_t>(IntOf(m, "MemberId", 0));
        if (!p.member_id) continue;
        p.online_id = StrOf(m, "OnlineId");
        p.addr = AddrOf(StrOf(m, "Addr"));
        p.port = static_cast<std::uint16_t>(IntOf(m, "Port", 0));
        p.local_addr = AddrOf(StrOf(m, "LocalAddr"));
        p.local_port = static_cast<std::uint16_t>(IntOf(m, "LocalPort", 0));
        // The server's own rule, applied here too: a STUN-mapped address that differs from the
        // peer's local one means a NAT in between, and only the mapped one reaches the peer.
        const std::uint32_t mapped = AddrOf(StrOf(m, "MappedAddr"));
        const long long mapped_port = IntOf(m, "MappedPort", 0);
        if (mapped && mapped_port > 0 && mapped_port <= 65535 && mapped != p.local_addr &&
            mapped != p.addr) {
            p.addr = mapped;
            p.port = static_cast<std::uint16_t>(mapped_port);
        }
        if (!p.addr) {
            p.addr = p.local_addr;
            p.port = p.local_port;
        }
        PeersUpsert(p);
        ++n;
    }
    return n;
}
Peer PeersProvisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    Peer before, after;
    {
        std::lock_guard lk(g_pmu);
        bool found = false;
        for (auto& [m, p] : g_peers) {
            if (p.online_id == id && m >= 0xff00) {
                before = p;
                p.addr = addr;
                p.port = port;
                after = p;
                found = true;
                break;
            }
        }
        if (!found) {
            std::uint16_t m = 0xff00;
            while (g_peers.count(m)) ++m;
            after.member_id = m;
            after.online_id = id;
            after.addr = addr;
            after.port = port;
            g_peers[m] = after;
        }
    }
    PunchIfNew(before, after);
    return after;
}

// ---- STUN: the reflexive address of our P2P port ---------------------------------------------------

namespace {
struct Mapped {
    std::mutex mu;
    std::string addr;
    int port = 0;
    std::int64_t at_ms = 0;
    bool asked = false;
};
Mapped g_mapped;

void StunServer(std::string* host, std::uint16_t* port) {
    std::string spec = Cfg().stun;
    if (spec.empty()) spec = HostOf(Cfg().np_base);
    *port = 3478;
    if (const auto colon = spec.find(':'); colon != std::string::npos) {
        *port = static_cast<std::uint16_t>(std::atoi(spec.c_str() + colon + 1));
        spec = spec.substr(0, colon);
    }
    *host = spec;
}

// At context_start, then when the answer is over two minutes old (the keepalive asks every 15 s).
void MappedRefresh(bool force) {
    std::lock_guard lk(g_mapped.mu);
    if (!force && g_mapped.asked && NowMs() - g_mapped.at_ms < 120000) return;
    g_mapped.asked = true;
    g_mapped.at_ms = NowMs();
    if (Cfg().stun == "off") return;
    std::string host;
    std::uint16_t sport = 0;
    StunServer(&host, &sport);
    if (host.empty()) return;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    bool ok = false;
    Relay relay;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        const auto t0 = Clock::now();
        ok = P2pStun(host.c_str(), sport, 1000, &addr, &port, &relay);
        if (ok) {
            g_stun_rtt_us = static_cast<int>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        }
    }
    if (!ok) {
        if (!g_mapped.addr.empty()) return; // keep the last answer
        Log("stun: no answer from %s:%u; the server gets the LAN address only", host.c_str(), sport);
        return;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", addr & 0xff, (addr >> 8) & 0xff,
                  (addr >> 16) & 0xff, addr >> 24);
    if (g_mapped.addr != text || g_mapped.port != port) {
        Log("stun: %s:%u sees our P2P port %d as %s:%u", host.c_str(), sport, P2pPort(), text, port);
    }
    g_mapped.addr = text;
    g_mapped.port = port;
}

Json OurEndpoint() {
    MappedRefresh(false);
    Json o = Json::object();
    o["OnlineId"] = OnlineId();
    o["LocalAddr"] = LocalAddrText();
    o["LocalPort"] = P2pPort();
    o["PublicAddr"] = LocalAddrText();
    o["PublicPort"] = P2pPort();
    std::lock_guard lk(g_mapped.mu);
    o["MappedAddr"] = g_mapped.addr;
    o["MappedPort"] = g_mapped.port;
    return o;
}

std::mutex g_roommu;
std::string g_room_sid;
int g_room_mid = 0;
} // namespace

// ---- the session service's calls ----------------------------------------------------------------

bool ServerSignalingResolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port,
                            std::string& error) {
    Json body = {{"OnlineId", id}};
    Json reply;
    if (!NpPost("/np/signaling/resolve", body, reply, error)) return false;
    if (IntOf(reply, "ResKind", -1) != 0) {
        error = "not registered";
        return false;
    }
    const std::uint32_t a = AddrOf(StrOf(reply, "Addr"));
    const long long p = IntOf(reply, "Port", 0);
    if (!a || p <= 0 || p > 65535) {
        error = "no address";
        return false;
    }
    if (addr) *addr = a;
    if (port) *port = static_cast<std::uint16_t>(p);
    return true;
}

bool ServerContextStart(Json& reply, std::string& error) {
    MappedRefresh(true);
    Json body = Json::object();
    body["OnlineId"] = OnlineId();
    body["SignalingAddr"] = LocalAddrText();
    body["SignalingPort"] = P2pPort();
    {
        std::lock_guard lk(g_mapped.mu);
        body["MappedAddr"] = g_mapped.addr;
        body["MappedPort"] = g_mapped.port;
    }
    return NpPost("/mp/matching2/context_start", body, reply, error);
}

bool ServerCreateRoom(int max_members, const Json& extra, Json& reply, std::string& error) {
    Json body = OurEndpoint();
    body["MaxMembers"] = max_members;
    if (extra.is_object()) {
        for (const auto& [k, v] : extra.items()) body[k] = v;
    }
    return NpPost("/mp/matching2/create_room", body, reply, error);
}

bool ServerJoinRoom(std::uint64_t room_id, Json& reply, std::string& error) {
    Json body = OurEndpoint();
    body["RoomId"] = static_cast<long long>(room_id);
    return NpPost("/mp/matching2/join_room", body, reply, error);
}

bool ServerLeaveRoom(const std::string& session_id, int member_id, Json& reply,
                     std::string& error) {
    Json body = {{"SessionId", session_id}, {"MemberId", member_id}};
    return NpPost("/mp/matching2/leave_room", body, reply, error);
}

int ServerHeartbeat(const std::string& session_id, int member_id) {
    Json body = {{"SessionId", session_id}, {"MemberId", member_id}};
    Json reply;
    std::string err;
    if (!NpPost("/mp/matching2/heartbeat", body, reply, err, 3000)) return -1;
    return IntOf(reply, "InRoom", 1) == 0 ? 0 : 1;
}

bool ServerKickMember(const std::string& session_id, int member_id, int kicker_id,
                      const std::uint8_t* opt, std::size_t opt_len, std::string& error) {
    static const char* const b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    opt_len = std::min<std::size_t>(opt_len, 16);
    for (std::size_t i = 0; i < opt_len; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(opt[i]) << 16) |
                                (i + 1 < opt_len ? static_cast<std::uint32_t>(opt[i + 1]) << 8 : 0u) |
                                (i + 2 < opt_len ? opt[i + 2] : 0u);
        enc += b64[(v >> 18) & 63];
        enc += b64[(v >> 12) & 63];
        enc += i + 1 < opt_len ? b64[(v >> 6) & 63] : '=';
        enc += i + 2 < opt_len ? b64[v & 63] : '=';
    }
    Json body = {{"SessionId", session_id},
                 {"MemberId", member_id},
                 {"KickerMemberId", kicker_id},
                 {"OptData", enc}};
    Json reply;
    return NpPost("/mp/matching2/kick_member", body, reply, error, 4000);
}

void RoomSet(const std::string& session_id, int member_id) {
    std::lock_guard lk(g_roommu);
    g_room_sid = session_id;
    g_room_mid = member_id;
}
void RoomClear() {
    RoomSet("", 0);
}

// ---- the event poller and the keepalive -------------------------------------------------------------

namespace {
std::mutex g_pollmu;
std::condition_variable g_pollcv;
std::atomic<bool> g_pollrun{false};
// Each start gets a new generation: a poller of an earlier one (still waiting on a long poll
// after a stop) ends at its answer instead of polling beside the new one.
std::atomic<std::uint64_t> g_pollgen{0};
std::function<void(const Json&)> g_handler;
long long g_cursor = 0;

// Long polling (The Hunter's Dream): the server holds an empty poll up to WaitMs and answers
// it the moment an event for this player arrives, so invites come at once and the clients no
// longer ask four times a second. Every answer of such a server carries WaitMaxMs.
constexpr int kPollWaitMs = 25000;

int Jitter(int ms) {
    thread_local std::mt19937 random{std::random_device{}()};
    return static_cast<int>(ms * std::uniform_real_distribution<double>(0.8, 1.2)(random));
}

void PollerMain(std::uint64_t generation) {
    const std::string me = OnlineId();
    const auto current = [generation] {
        return g_pollrun.load() && g_pollgen.load() == generation;
    };
    int wait_ms = kPollWaitMs; // from the first poll; dropped for a server that does not hold polls
    int backoff_ms = 0;
    int pause_ms = 0;
    while (current()) {
        Json body = {{"OnlineId", me}, {"Cursor", g_cursor}, {"Categories", Json::array({"matching2"})}};
        if (wait_ms > 0) body["WaitMs"] = wait_ms;
        Json reply;
        std::string err;
        // The read waits past the server's wait: a shorter one made every empty poll an error.
        const int timeout_ms = wait_ms > 0 ? wait_ms + 5000 : 4000;
        // BB_THD_POLL_LOG=1: each poll's wait, time to its answer and result (diagnosis).
        static const bool poll_log = [] {
            const char* env = std::getenv("BB_THD_POLL_LOG");
            return env && env[0] == '1';
        }();
        const auto poll_start = std::chrono::steady_clock::now();
        const bool answered = NpPost("/np/events/poll", body, reply, err, timeout_ms);
        if (poll_log) {
            const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - poll_start)
                                       .count();
            Log("np: poll WaitMs %d -> %s in %lld ms (WaitMaxMs %lld, HasEvent %lld)", wait_ms,
                answered ? "answer" : err.c_str(), took, IntOf(reply, "WaitMaxMs", 0),
                IntOf(reply, "HasEvent", 0));
        }
        if (answered) {
            if (!current()) break; // stopped or restarted while this poll waited
            backoff_ms = 0;
            const long long max_wait = IntOf(reply, "WaitMaxMs", 0);
            wait_ms = max_wait > 0 ? static_cast<int>(std::min<long long>(kPollWaitMs, max_wait)) : 0;
            const long long next = IntOf(reply, "NextCursor", g_cursor);
            if (IntOf(reply, "HasEvent", 0) != 0) {
                std::function<void(const Json&)> handler;
                {
                    std::lock_guard lk(g_pollmu);
                    handler = g_handler;
                }
                Log("np: event #%lld %s", IntOf(reply, "EventId", 0), StrOf(reply, "Name").c_str());
                if (handler) {
                    try {
                        handler(reply);
                    } catch (const std::exception& e) {
                        Log("np: event handler: %s", e.what());
                    }
                }
                Json ack = {{"OnlineId", me}, {"Cursor", next}};
                Json ack_reply;
                std::string ack_err;
                NpPost("/np/events/ack", ack, ack_reply, ack_err, 4000);
                g_cursor = next;
                continue; // drain without waiting while there are events
            }
            g_cursor = std::max(g_cursor, next);
            if (wait_ms > 0) continue; // the server waited: poll again at once
            pause_ms = 250;            // a server without long polling: the old pacing
        } else if (!current()) {
            break;
        } else if (Refused()) {
            pause_ms = 60000; // the server refused the sign-in: nothing arrives until it changes
        } else {
            // Network error, timeout, 5xx or 429: 1, 2, 4 ... 30 s with jitter; reset on success.
            backoff_ms = backoff_ms ? std::min(backoff_ms * 2, 30000) : 1000;
            pause_ms = Jitter(backoff_ms);
        }
        std::unique_lock lk(g_pollmu);
        g_pollcv.wait_for(lk, std::chrono::milliseconds(pause_ms), [&] { return !current(); });
    }
}

std::atomic<bool> g_keeprun{false};
std::mutex g_keepmu;
std::condition_variable g_keepcv;

// Our new reflexive address to the server (a relay port that changed).
void ServerSignalingUpdate() {
    Json body = Json::object();
    body["OnlineId"] = OnlineId();
    {
        std::lock_guard lk(g_mapped.mu);
        body["MappedAddr"] = g_mapped.addr;
        body["MappedPort"] = g_mapped.port;
    }
    {
        std::lock_guard lk(g_roommu);
        body["SessionId"] = g_room_sid;
        body["MemberId"] = g_room_mid;
    }
    Json reply;
    std::string err;
    NpPost("/mp/matching2/signaling_update", body, reply, err, 4000);
}

// The STUN binding every 15 s while the context runs: it keeps our NAT's mapping to the server
// open (routers forget an idle UDP mapping in 30 s to a few minutes) and the relay port alive.
void KeepaliveMain() {
    std::string last;
    {
        std::lock_guard lk(g_mapped.mu);
        last = g_mapped.addr + ":" + std::to_string(g_mapped.port);
    }
    while (g_keeprun.load()) {
        {
            std::unique_lock lk(g_keepmu);
            g_keepcv.wait_for(lk, std::chrono::seconds(15), [] { return !g_keeprun.load(); });
        }
        if (!g_keeprun.load()) break;
        MappedRefresh(true);
        std::string now;
        {
            std::lock_guard lk(g_mapped.mu);
            now = g_mapped.addr + ":" + std::to_string(g_mapped.port);
        }
        if (now != last && now != ":0") {
            Log("stun: our address is now %s (was %s); telling the server", now.c_str(), last.c_str());
            ServerSignalingUpdate();
            last = now;
        }
    }
}
} // namespace

void PollerStart(std::function<void(const Json&)> handler) {
    std::lock_guard lk(g_pollmu);
    g_handler = std::move(handler);
    if (g_pollrun.exchange(true)) return;
    g_cursor = 0;
    std::thread(PollerMain, g_pollgen.fetch_add(1) + 1).detach();
    if (!g_keeprun.exchange(true)) std::thread(KeepaliveMain).detach();
}

void PollerStop() {
    {
        std::lock_guard lk(g_keepmu);
        g_keeprun.store(false);
        g_keepcv.notify_all();
    }
    std::lock_guard lk(g_pollmu);
    g_pollrun.store(false);
    g_pollcv.notify_all();
}

// ---- game memory: 1.09's addresses (Binary Ninja, image base 0x400000) -------------------------------

namespace {
constexpr std::uint64_t kImageBase = 0x400000;
constexpr std::uint64_t kCharaStep = 0x2043ee0;      // the login's character step (1.09's check)
constexpr std::uint64_t kSignalingGate = 0x586cc09;  // u8: the NpMatching2 callbacks' gate
constexpr std::uint64_t kFrpgNetMan = 0x593b120;     // FrpgNetMan* (the singleton's slot)
constexpr std::uint32_t kAreaOff = 0xa7c, kLevelOff = 0xaa8, kPosOff = 0xaac;
const std::uint8_t kCharaStepPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                           0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
std::uint8_t* g_image = nullptr;

template <typename T>
bool ReadGuest(std::uint64_t at, T* out) {
    return at > 0x10000 && BbEngine::host_read_safe(reinterpret_cast<const void*>(at), out, sizeof(T));
}
} // namespace

void SetImage(std::uint8_t* image, std::uint64_t size) {
    g_image = nullptr;
    if (!image || size < kFrpgNetMan + 8 - kImageBase) return;
    if (std::memcmp(image + (kCharaStep - kImageBase), kCharaStepPrologue,
                    sizeof(kCharaStepPrologue)) != 0) {
        return; // not 1.09: no game memory is touched
    }
    g_image = image;
}

// The game's matching callbacks queue events only while this byte is 1; on PSN it was set for it.
void SetSignalingGate() {
    if (!g_image) return;
    auto* gate = g_image + (kSignalingGate - kImageBase);
    if (*gate != 1) {
        *gate = 1;
        Log("np: signaling gate set");
    }
}

// The host's area, level and position for the invite the server relays to the summoned player.
void HostExtra(Json& extra) {
    if (!g_image) return;
    std::uint64_t man = 0;
    std::uint32_t area = 0, level = 0;
    float pos[3] = {};
    if (!ReadGuest(reinterpret_cast<std::uint64_t>(g_image + (kFrpgNetMan - kImageBase)), &man) ||
        man <= 0x10000 || !ReadGuest(man + kAreaOff, &area) || !ReadGuest(man + kLevelOff, &level) ||
        !BbEngine::host_read_safe(reinterpret_cast<const void*>(man + kPosOff), pos, sizeof(pos))) {
        return;
    }
    extra["HostArea"] = area;
    extra["HostLevel"] = level;
    extra["HostPos"] = Json::array({pos[0], pos[1], pos[2]});
    extra["MemberTag"] = 1;
}

// ---- SceNpId: the online id in its first 16 bytes --------------------------------------------------

void FillNpId(void* out, const char* online) {
    if (!out) return;
    std::memset(out, 0, 36);
    if (!online || !online[0]) online = "Player";
    std::snprintf(static_cast<char*>(out), 17, "%s", online);
}

std::string NpIdText(const void* npid) {
    if (!npid) return "";
    char buf[17]{};
    std::memcpy(buf, npid, 16);
    return buf;
}

void Register(Core::Loader::SymbolsResolver* sym) {
    RegisterNet(sym);
    RegisterMatching2(sym);
    RegisterNp(sym);
}

} // namespace BbThd

// The sceHttp hook (vendor/shadps4/core/libraries/network/http.cpp).
extern "C" bool bbthd_enabled() {
    return BbThd::Enabled();
}
extern "C" void bbthd_add_game_headers(std::vector<std::pair<std::string, std::string>>* headers) {
    if (headers) BbThd::AddGameHeaders(*headers);
}
