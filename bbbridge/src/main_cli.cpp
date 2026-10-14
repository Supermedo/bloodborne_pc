// bbbridge-cli: the bridge without a window (tests, servers, troubleshooting).
//   bbbridge-cli.exe [--config bbbridge_config.json] [--log bbbridge.log]
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>

#include "account_link.hpp"
#include "bridge.hpp"
#include "setup.hpp"

namespace {
std::atomic<bool> g_quit{false};
BOOL WINAPI on_ctrl(DWORD) {
    g_quit = true;
    return TRUE;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring config_path = L"bbbridge_config.json", log_path, web_code_npid;
    bool link = false;
    std::wstring route, bbport_dir;
    struct { std::wstring op, name, code; } account_op;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == L"--log" && i + 1 < argc) log_path = argv[++i];
        else if (a == L"--link-discord") link = true;
        else if (a == L"--route" && i + 1 < argc) route = argv[++i];
        else if (a == L"--bbport" && i + 1 < argc) bbport_dir = argv[++i];
        else if (a == L"--web-code" && i + 1 < argc) web_code_npid = argv[++i];
        else if (a == L"--create" && i + 1 < argc) account_op = {L"create", argv[++i], L""};
        else if (a == L"--recover" && i + 2 < argc) {
            account_op = {L"recover", argv[i + 1], argv[i + 2]};
            i += 2;
        } else if (a == L"--sign-out" && i + 1 < argc) account_op = {L"sign-out", argv[++i], L""};
        else if (a == L"--version") {
            std::printf("bbbridge %s\n", bb::kVersion);
            return 0;
        } else {
            std::printf("usage: bbbridge-cli [--config file.json] [--log file.log] [--version]\n"
                        "                    [--link-discord] [--web-code NPID] [--create NAME]\n"
                        "                    [--recover NAME CODE] [--sign-out NAME]\n"
                        "                    [--route on|off [--bbport DIR]]\n");
            return 2;
        }
    }
    if (!log_path.empty()) bb::set_log_file(log_path);
    bb::json j = bb::json::object();
    std::ifstream in(config_path);
    if (in) {
        j = bb::json::parse(in, nullptr, false);
        if (j.is_discarded()) {
            std::printf("config %s is not valid JSON\n", bb::narrow(config_path).c_str());
            return 1;
        }
    }
    const bb::Config cfg = bb::Config::from_json(j);
    if (link) {   // sign in with Discord, store the account, exit
        bb::DiscordLink dl(cfg.auth_server, bb::widen(cfg.state_file));
        dl.start();
        bool shown = false;
        while (dl.state() == bb::DiscordLink::State::Starting || dl.state() == bb::DiscordLink::State::Waiting) {
            if (!shown && dl.state() == bb::DiscordLink::State::Waiting) {
                std::printf("Open %s and enter the code %s\n", dl.uri().c_str(), dl.user_code().c_str());
                std::fflush(stdout);
                shown = true;
            }
            Sleep(200);
        }
        if (dl.state() != bb::DiscordLink::State::Approved) {
            std::printf("failed: %s\n", dl.message().c_str());
            return 1;
        }
        std::printf("signed in as %s\n", dl.online_id().c_str());
        return 0;
    }
    if (!route.empty()) {   // point bbport at the bridge (on) or back (off), as the app's checkbox
        try {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            bb::BbportSetup setup(bb::fs::path(self).parent_path(), bbport_dir);
            if (route == L"on") setup.enable();
            else setup.disable();
            std::printf("ok: bbport %s, routed=%d\n", bb::narrow(setup.bbport.wstring()).c_str(), setup.routed() ? 1 : 0);
            return 0;
        } catch (const std::exception& e) {
            std::printf("failed: %s\n", e.what());
            return 1;
        }
    }
    if (!account_op.op.empty()) {
        const std::wstring state = bb::widen(cfg.state_file);
        const std::string name = bb::narrow(account_op.name);
        const std::string err =
            account_op.op == L"create"    ? bb::account_create_now(cfg.auth_server, state, name)
            : account_op.op == L"recover" ? bb::account_recover_now(cfg.auth_server, state, name, bb::narrow(account_op.code))
                                          : bb::account_sign_out(cfg.auth_server, state, name);
        std::printf("%s\n", err.empty() ? "ok" : ("failed: " + err).c_str());
        return err.empty() ? 0 : 1;
    }
    if (!web_code_npid.empty()) {
        std::string error;
        const std::string code = bb::website_code(cfg.auth_server, bb::widen(cfg.state_file), bb::narrow(web_code_npid), error);
        if (code.empty()) {
            std::printf("failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("%s\n", code.c_str());
        return 0;
    }
    bb::log(bb::fmt("bbbridge %s (cli)", bb::kVersion));
    bb::Bridge bridge(cfg);
    std::string error;
    if (!bridge.start(&error)) return 1;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    while (!g_quit) Sleep(200);
    bb::log("stopping");
    bridge.stop();
    return 0;
}
