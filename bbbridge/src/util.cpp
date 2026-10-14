#include "util.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>

namespace bb {

namespace {
std::mutex g_log_mu;
std::function<void(const std::string&)> g_sink;
std::unique_ptr<std::ofstream> g_file;
}  // namespace

std::string fmt(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, format, args);
    va_end(args);
    return out;
}

void log(const std::string& msg) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    const std::string line = std::string(stamp) + " [bbbridge] " + msg;
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_file && *g_file) {
        *g_file << line << '\n';
        g_file->flush();
    }
    if (g_sink) g_sink(line);
}

void set_log_sink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_sink = std::move(sink);
}

void set_log_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::wstring prev = path;
    const size_t dot = prev.rfind(L".log");
    prev = (dot == std::wstring::npos ? prev : prev.substr(0, dot)) + L".prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::trunc);
}

double now() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool net_init() {
    static bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

sockaddr_in to_sockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(e.port));
    inet_pton(AF_INET, e.ip.c_str(), &a.sin_addr);
    return a;
}

Endpoint from_sockaddr(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return Endpoint{ip, ntohs(a.sin_port)};
}

std::string detect_lan_addr() {
    // The interface the OS would route to the internet through (no packet is sent).
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in target = to_sockaddr(Endpoint{"8.8.8.8", 80});
    std::string out = "127.0.0.1";
    if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) out = from_sockaddr(local).ip;
    }
    closesocket(s);
    return out;
}

std::string resolve_ipv4(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
    const std::string ip = from_sockaddr(*reinterpret_cast<sockaddr_in*>(res->ai_addr)).ip;
    freeaddrinfo(res);
    return ip;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace bb
