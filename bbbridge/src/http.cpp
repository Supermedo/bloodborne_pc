#include "http.hpp"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <sstream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace http {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

HINTERNET open_session() {
    return WinHttpOpen(L"bbbridge", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0);
}

HINTERNET session() {
    static HINTERNET h = open_session();
    return h;
}

ClientResponse send_on(HINTERNET session, const ClientRequest& req);

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Status";
    }
}

}  // namespace

std::string header_value(const Headers& headers, const std::string& name) {
    for (const auto& [k, v] : headers)
        if (iequals(k, name)) return v;
    return "";
}

ClientResponse send(const ClientRequest& req) { return send_on(session(), req); }

Pool::~Pool() {
    if (session_) WinHttpCloseHandle(session_);
}

ClientResponse Pool::send(const ClientRequest& req) {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_ && bb::now() - opened_ >= max_age_) {   // closing the session closes its connection
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
    if (!session_) {
        session_ = open_session();
        opened_ = bb::now();
    }
    return send_on(session_, req);
}

namespace {

ClientResponse send_on(HINTERNET session, const ClientRequest& req) {
    ClientResponse res;
    if (!session) {
        res.error = "WinHttpOpen failed";
        return res;
    }
    const std::wstring wurl = bb::widen(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.error = "bad url " + req.url;
        return res;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    Handle conn{WinHttpConnect(session, host, uc.nPort, 0)};
    if (!conn.h) {
        res.error = bb::fmt("connect failed (%lu)", GetLastError());
        return res;
    }
    const std::wstring object = std::wstring(path) + extra;
    Handle request{WinHttpOpenRequest(conn.h, bb::widen(req.method).c_str(), object.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h) {
        res.error = bb::fmt("open request failed (%lu)", GetLastError());
        return res;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpSetTimeouts(request.h, req.timeout_ms, req.timeout_ms, req.timeout_ms, req.timeout_ms);
    std::wstring headers;
    for (const auto& [k, v] : req.headers) headers += bb::widen(k + ": " + v + "\r\n");
    if (!headers.empty())
        WinHttpAddRequestHeaders(request.h, headers.c_str(), static_cast<DWORD>(-1L),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    const DWORD len = static_cast<DWORD>(req.body.size());
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            len ? const_cast<char*>(req.body.data()) : WINHTTP_NO_REQUEST_DATA, len, len, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        const DWORD e = GetLastError();
        res.error = e == ERROR_WINHTTP_TIMEOUT ? "The read operation timed out" : bb::fmt("request failed (%lu)", e);
        return res;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (size) {
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
            std::istringstream lines(bb::narrow(raw));
            std::string line;
            bool first = true;
            while (std::getline(lines, line)) {
                if (first) { first = false; continue; }   // status line
                const size_t c = line.find(':');
                if (c != std::string::npos) res.headers.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
            }
        }
    }
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            res.error = bb::fmt("read failed (%lu)", GetLastError());
            return res;
        }
        if (!avail) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), avail, &read)) {
            res.error = bb::fmt("read failed (%lu)", GetLastError());
            return res;
        }
        res.body.append(chunk.data(), read);
    }
    return res;
}

}  // namespace

ServerResponse ServerResponse::json(const bb::json& j, int status) {
    ServerResponse r;
    r.status = status;
    r.body = j.dump();
    r.headers.emplace_back("Content-Type", "application/json");
    return r;
}

// ---- server ----

bool Server::start(const std::string& host, int port, Handler handler, std::string* error) {
    bb::net_init();
    handler_ = std::move(handler);
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = bb::to_sockaddr(bb::Endpoint{host, port});
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_, 16) != 0) {
        if (error) *error = bb::fmt("cannot listen on %s:%d (error %d)", host.c_str(), port, WSAGetLastError());
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    int len = sizeof(addr);
    getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    running_ = true;
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
    if (acceptor_.joinable()) acceptor_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (SOCKET s : clients_) shutdown(s, SD_BOTH);
    }
    // Connection threads are detached (the game opens one per request); wait
    // for the ones still running, which a shutdown socket ends quickly.
    for (int i = 0; i < 300 && active_.load() > 0; ++i) Sleep(10);
}

void Server::accept_loop() {
    while (running_) {
        SOCKET s = accept(listen_, nullptr, nullptr);
        if (s == INVALID_SOCKET) break;
        {
            std::lock_guard<std::mutex> lk(mu_);
            clients_.insert(s);
        }
        ++active_;
        std::thread([this, s] {
            serve(s);
            --active_;
        }).detach();
    }
}

void Server::serve(SOCKET s) {
    std::string buf;
    char chunk[8192];
    bool keep = true;
    while (keep && running_) {
        size_t end;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            const int n = recv(s, chunk, sizeof(chunk), 0);
            if (n <= 0) { keep = false; break; }
            buf.append(chunk, n);
            if (buf.size() > (1 << 20)) { keep = false; break; }
        }
        if (!keep) break;
        ServerRequest req;
        std::istringstream head(buf.substr(0, end));
        std::string line, version;
        std::getline(head, line);
        {
            std::istringstream rl(line);
            rl >> req.method >> req.target >> version;
        }
        while (std::getline(head, line)) {
            const size_t c = line.find(':');
            if (c != std::string::npos) req.headers.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
        }
        buf.erase(0, end + 4);
        size_t want = 0;
        const std::string cl = header_value(req.headers, "Content-Length");
        if (!cl.empty()) {
            try { want = static_cast<size_t>(std::stoull(cl)); } catch (...) { want = 0; }
        }
        while (buf.size() < want) {
            const int n = recv(s, chunk, sizeof(chunk), 0);
            if (n <= 0) { keep = false; break; }
            buf.append(chunk, n);
        }
        if (!keep) break;
        req.body = buf.substr(0, want);
        buf.erase(0, want);
        if (iequals(header_value(req.headers, "Connection"), "close") || version == "HTTP/1.0") keep = false;

        ServerResponse res;
        try {
            res = handler_(req);
        } catch (const std::exception& e) {
            res = ServerResponse::json({{"ResKind", 1}, {"Message", std::string("bridge error: ") + e.what()}}, 500);
        }
        std::string out = bb::fmt("HTTP/1.1 %d %s\r\n", res.status, reason(res.status));
        for (const auto& [k, v] : res.headers)
            if (!iequals(k, "Content-Length") && !iequals(k, "Connection")) out += k + ": " + v + "\r\n";
        out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
        out += keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        if (req.method != "HEAD") out += res.body;
        size_t sent = 0;
        while (sent < out.size()) {
            const int n = ::send(s, out.data() + sent, static_cast<int>(out.size() - sent), 0);
            if (n <= 0) { keep = false; break; }
            sent += static_cast<size_t>(n);
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(s);
    }
    closesocket(s);
}

}  // namespace http
