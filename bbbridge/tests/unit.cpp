// Byte-level unit tests: the same checks as the Python bridge's test_p2p.py
// part 1, plus protobuf and the account id (values the Python bridge produced).
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "p2p.hpp"
#include "proto3.hpp"
#include "setup.hpp"
#include "shadnet.hpp"

using namespace bb;
using namespace bb::p2p;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s  %s\n", name, detail.c_str());
    }
}

static std::string hex(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += fmt("%02x", c);
    return out;
}

static std::string be16s(uint16_t v) { return std::string{static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; }

static std::string handshake(uint8_t kind, const std::string& from, const std::string& to, uint32_t from_conn,
                             uint64_t nonce) {
    std::string p = "SHAD";
    p.push_back(static_cast<char>(T_HANDSHAKE));
    p.push_back(static_cast<char>(kind));
    std::string f = from, t = to;
    f.resize(16, '\0');
    t.resize(16, '\0');
    p += f + t;
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<char>((from_conn >> (8 * i)) & 0xFF));
    p += std::string(12, '\0');
    for (int i = 0; i < 8; ++i) p.push_back(static_cast<char>((nonce >> (8 * i)) & 0xFF));
    return p;
}

static std::string m2(uint8_t kind, uint64_t room, uint16_t from_member, uint16_t to_member, const std::string& from,
                      uint64_t nonce) {
    std::string p = "SHAD";
    p.push_back(static_cast<char>(T_M2));
    p.push_back(static_cast<char>(kind));
    for (int i = 0; i < 8; ++i) p.push_back(static_cast<char>((room >> (8 * i)) & 0xFF));
    p.push_back(static_cast<char>(from_member & 0xFF));
    p.push_back(static_cast<char>(from_member >> 8));
    p.push_back(static_cast<char>(to_member & 0xFF));
    p.push_back(static_cast<char>(to_member >> 8));
    std::string f = from;
    f.resize(16, '\0');
    p += f + std::string(8, '\0');
    for (int i = 0; i < 8; ++i) p.push_back(static_cast<char>((nonce >> (8 * i)) & 0xFF));
    return p;
}

static std::string control(uint8_t kind, const std::string& from, const std::string& to) {
    std::string p = "SHAD";
    p.push_back(static_cast<char>(T_CONTROL));
    p.push_back(static_cast<char>(kind));
    std::string f = from, t = to;
    f.resize(16, '\0');
    t.resize(16, '\0');
    p += f + t + std::string(12, '\0');
    return p;
}

template <typename T>
static T le(const std::string& s, size_t at) {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[at + i])) << (8 * i);
    return static_cast<T>(v);
}

int main() {
    std::printf("== bbbridge unit ==\n");
    std::string out;

    // framing
    check("port->bbhost one-byte vports",
          port_to_ps4(be16s(40) + be16s(30) + "game", out) == Frame::Game && out == std::string("\xff\xc3\x28\x1e") + "game",
          hex(out));
    check("bbhost->port one-byte vports",
          ps4_to_port(std::string("\xff\xc3\x1e\x28") + "data", out) && out == be16s(30) + be16s(40) + "data", hex(out));
    std::string two;
    port_to_ps4(be16s(3000) + be16s(40) + "x", two);
    check("two-byte vports round trip", ps4_to_port(two, out) && out == be16s(3000) + be16s(40) + "x", hex(two));
    check("comid flag skipped",
          ps4_to_port(std::string("\xff\xe3") + "ABCD" + "\x28\x1e" + "z", out) && out == be16s(40) + be16s(30) + "z");
    check("probe is not game data", !ps4_to_port(kProbe, out));
    check("port signaling channel detected",
          port_to_ps4(be16s(0xFFFF) + be16s(0xFFFF) + "SHAD\x05", out) == Frame::Signaling && out == "SHAD\x05");
    const std::string token = "T0K3N!!!";
    const std::string wrapped = relay_wrap(token, 41000, std::string("\xff\xc3\x28\x1e"));
    check("relay frame to server", wrapped.substr(0, 2) == "\xfbR" && wrapped.substr(2, 8) == token &&
                                       wrapped.substr(10, 2) == be16s(41000) && wrapped.size() == 16);
    int port = 0;
    check("relay frame from server",
          relay_unwrap(std::string("\xfbr") + be16s(41001) + "dg", port, out) && port == 41001 && out == "dg");

    // STUN
    std::string txid;
    const std::string req = stun_request("", txid);
    check("stun request shape", req.substr(0, 4) == be16s(1) + be16s(16) && req.substr(4, 4) == "\x21\x12\xa4\x42" &&
                                    req.substr(20, 4) == std::string("\x81\x00\x00\x0c", 4) && req.substr(24, 4) == "bbr1");
    const uint32_t addr = (5u << 24) | (6u << 16) | (7u << 8) | 8u;
    const uint32_t xaddr = addr ^ ((static_cast<uint8_t>(txid[0]) << 24) | (static_cast<uint8_t>(txid[1]) << 16) |
                                   (static_cast<uint8_t>(txid[2]) << 8) | static_cast<uint8_t>(txid[3]));
    const uint16_t xport = static_cast<uint16_t>(4321 ^ ((static_cast<uint8_t>(txid[0]) << 8) | static_cast<uint8_t>(txid[1])));
    std::string attr = be16s(0x0020) + be16s(8) + std::string("\x00\x01", 2) + be16s(xport) + be16s(xaddr >> 16) +
                       be16s(xaddr & 0xFFFF);
    const std::string resp = be16s(0x0101) + be16s(static_cast<uint16_t>(attr.size())) + txid + attr;
    StunAnswer a;
    check("stun xor-mapped parsed", stun_parse(resp, txid, a) && a.mapped.ip == "5.6.7.8" && a.mapped.port == 4321,
          a.mapped.str());
    check("stun wrong txid rejected", !stun_parse(resp, std::string(16, '\0'), a));

    // signaling, answered for the bbhost peer
    PeerSignaling ps("hunter", 2, "porthost", 1, 777);
    auto r = ps.handle(handshake(HS_OFFER, "porthost", "hunter", 5, 0));
    check("Offer -> Accept from the peer",
          r.size() == 1 && r[0].size() == 62 && static_cast<uint8_t>(r[0][5]) == HS_ACCEPT &&
              r[0].compare(6, 6, "hunter") == 0 && r[0].compare(22, 8, "porthost") == 0 && le<uint32_t>(r[0], 42) == 5 &&
              le<uint32_t>(r[0], 46) == 0);
    r = ps.handle(handshake(HS_CHECK, "porthost", "hunter", 7, 123456));
    check("Check -> CheckAck echoes nonce",
          r.size() == 1 && static_cast<uint8_t>(r[0][5]) == HS_CHECK_ACK && le<uint64_t>(r[0], 54) == 123456);
    r = ps.handle(control(CTL_ACTIVATION_REQUEST, "porthost", "hunter"));
    check("ActivationRequest -> ActivationAck", r.size() == 1 && r[0].size() == 50 && static_cast<uint8_t>(r[0][5]) == CTL_ACTIVATION_ACK);
    const auto first = ps.handle(control(CTL_ESTABLISHED, "porthost", "hunter"));
    const auto second = ps.handle(control(CTL_ESTABLISHED, "porthost", "hunter"));
    check("Established answered once", first.size() == 1 && second.empty());
    r = ps.handle(m2(HS_OFFER, 777, 1, 2, "porthost", 0));
    check("matching2 Offer -> Accept, member ids swapped",
          r.size() == 1 && r[0].size() == 50 && static_cast<uint8_t>(r[0][5]) == HS_ACCEPT && le<uint64_t>(r[0], 6) == 777 &&
              le<uint16_t>(r[0], 14) == 2 && le<uint16_t>(r[0], 16) == 1 && r[0].compare(18, 6, "hunter") == 0);
    r = ps.handle(m2(HS_CHECK, 777, 1, 2, "porthost", 99));
    check("matching2 Check -> CheckAck", r.size() == 1 && static_cast<uint8_t>(r[0][5]) == HS_CHECK_ACK && le<uint64_t>(r[0], 42) == 99);
    PeerSignaling provisional("guest", -29999, "porthost", 1, 777);
    const auto opening = provisional.opening();
    check("a provisional (negative) key opens without matching2",
          opening.size() == 1 && static_cast<uint8_t>(opening[0][5]) == CTL_ACTIVATION_REQUEST &&
              provisional.conn_id == 0x4242);
    r = ps.handle(std::string("SHAD\x05", 5) + std::string("\x03\x00\x00\x00", 4) + std::string("\x2b\x02\x00\x00\x00\x00\x00\x00", 8));
    check("echo ping -> pong", r.size() == 1 && r[0].size() == 17 && static_cast<uint8_t>(r[0][4]) == T_ECHO_PONG &&
                                   le<uint32_t>(r[0], 5) == 3 && le<uint64_t>(r[0], 9) == 555);

    // protobuf + shadNet framing
    pb::Writer w;
    w.s(1, "npid").u(2, 300).flag(4, true).u(5, 0).msg(6, "");
    pb::Message m(w.str());
    check("protobuf round trip", m.ok() && m.s(1) == "npid" && m.u(2) == 300 && m.u(4) == 1 && !m.has(5) && m.has(6));
    std::string blob;
    check("proto blob framing", shadnet::parse_proto_blob(shadnet::proto_blob("abc"), blob) && blob == "abc");
    shadnet::Header h;
    check("packet header", shadnet::parse_header(shadnet::build_reply(101, 7, 0, "xy"), h) && h.type == shadnet::Reply &&
                               h.command == 101 && h.size == 18 && h.packet_id == 7);

    // account id: the values the Python bridge gave these names
    check("account id matches the Python bridge (porthost)", account_id_for("porthost") == 4562636682122493ull,
          std::to_string(account_id_for("porthost")));
    check("account id matches the Python bridge (tester01)", account_id_for("tester01") == 4533768973479740ull,
          std::to_string(account_id_for("tester01")));

    // where bbport is found: chosen > the folder bbbridge is in > the launcher's game folder
    {
        const fs::path root = fs::temp_directory_path() / L"bbbridge-unit-find";
        std::error_code ec;
        fs::remove_all(root, ec);
        const fs::path bbport_dir = root / L"bloodborne", other = root / L"other", games = root / L"games";
        for (const auto& d : {bbport_dir / L"bbbridge", other, games / L"CUSA03173"}) fs::create_directories(d);
        std::ofstream(bbport_dir / L"Bloodborne.exe") << "x";
        std::ofstream(games / L"Play Bloodborne.exe") << "x";
        const std::string game_dir = narrow((games / L"CUSA03173").wstring());
        BbportSource src{};
        check("bbport: the folder bbbridge is in",
              find_bbport(bbport_dir / L"bbbridge", {}, game_dir, &src) == bbport_dir && src == BbportSource::ExeFolder);
        check("bbport: bbbridge.exe right next to Bloodborne.exe",
              find_bbport(bbport_dir, {}, "", &src) == bbport_dir && src == BbportSource::ExeFolder);
        check("bbport: a chosen folder that is gone falls back",
              find_bbport(bbport_dir / L"bbbridge", other, game_dir, &src) == bbport_dir && src == BbportSource::ExeFolder);
        check("bbport: the chosen folder wins",
              find_bbport(bbport_dir / L"bbbridge", games, "", &src) == games && src == BbportSource::Chosen);
        check("bbport: from the launcher's game folder",
              find_bbport(other / L"bbbridge", {}, game_dir, &src) == games && src == BbportSource::Launcher);
        check("bbport: not found",
              find_bbport(other / L"bbbridge", {}, "", &src).empty() && src == BbportSource::None);
        fs::remove_all(root, ec);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
