#include "setup.hpp"

#include <tlhelp32.h>

#include <fstream>
#include <sstream>

namespace bb {

namespace {

constexpr const char* kBackup = ".before-bbbridge";
constexpr const char* kMarker = "_bbbridge";
constexpr const char* kP2pVar = "SHADPS4_P2P_PORT";
constexpr int kP2pPort = 3658;
constexpr const char* kBridgeServer = "127.0.0.1:31313";
constexpr const char* kBridgeWebapi = "http://127.0.0.1:31315";
constexpr const char* kPublicServer = "srv.shadps4.net:31313";
constexpr const char* kPlaceholderPassword = "bbbridge";   // see enable()

// bbhost's play-log patch (bbhost src/engine/playlog.cpp) in bbport's patch
// format: the float immediates of `mov dword [rip+disp], imm32` in
// SprjPlaylogSystem's static initializer - 0x23F9B86 stores 300.0f (upload
// period), 0x23F9B68 stores 1.5f (position period); the float is at +6.
// Verified against eboot.bin 1.09.
const char* kPatchXml = R"xml(<?xml version="1.0"?>
<Patch>
    <TitleID>
        <ID>CUSA03173</ID>
    </TitleID>
    <Metadata Title="Bloodborne" Name="Live map: play log every 5 s, position every 0.5 s" Note="Uploads the play log every 5 s instead of 300 s and samples the position every 0.5 s instead of 1.5 s, as bbhost does, so the player shows on thehuntersdream.com's live map." Author="bbbridge (from bbhost)" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="float32" Address="0x023F9B8C" Value="5.0"/>
            <Line Type="float32" Address="0x023F9B6E" Value="0.5"/>
        </PatchList>
    </Metadata>
</Patch>
)xml";

nlohmann::ordered_json overrides() {
    return {{kMarker, "Written by bbbridge: the game's server traffic goes through bbbridge. Move bbbridge's "
                      "Online switch to shadNet to remove."},
            {"https://ss4.scej-network.jp:20443", "http://127.0.0.1:31316"},
            {"https://thehuntersdream.com:20443", "http://127.0.0.1:31316"},
            {"https://thehuntersdream.com:18671", "http://127.0.0.1:31317"},
            {"http://thehuntersdream.com:18671", "http://127.0.0.1:31317"},
            {"https://bb-playlog-test.s3.amazonaws.com:443", "http://127.0.0.1:31317"},
            {"https://bb-playlog-prod.s3.amazonaws.com:443", "http://127.0.0.1:31317"},
            {"https://thehuntersdream.com:18671/summon_messenger", "http://127.0.0.1:31315"},
            {"http://thehuntersdream.com:18671/summon_messenger", "http://127.0.0.1:31315"}};
}

using ojson = nlohmann::ordered_json;

ojson read_json(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return ojson();
    ojson j = ojson::parse(in, nullptr, false);
    return j.is_discarded() ? ojson() : j;
}

void write_json(const fs::path& p, const ojson& j) {
    const fs::path tmp = p.wstring() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2, ' ', false) << "\n";
    }
    MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void backup(const fs::path& p) {
    const fs::path target = p.wstring() + widen(kBackup);
    std::error_code ec;
    if (fs::exists(p, ec) && !fs::exists(target, ec)) fs::copy_file(p, target, ec);
}

std::string merge_env(const std::string& extra, const std::string& key, const std::string* value) {
    std::istringstream in(extra);
    std::string item, out;
    while (in >> item)
        if (item.rfind(key + "=", 0) != 0) out += (out.empty() ? "" : " ") + item;
    if (value) out += (out.empty() ? "" : " ") + key + "=" + *value;
    return out;
}

// Ours, or the Python bbbridge's (0.2.0: "_note": "bbbridge: ...").
bool ours(const fs::path& overrides_file) {
    const ojson j = read_json(overrides_file);
    return j.is_object() && (j.contains(kMarker) || jstr(j, "_note").rfind("bbbridge:", 0) == 0);
}

}  // namespace

bool process_running(const wchar_t* image) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, image) == 0;
    CloseHandle(snap);
    return found;
}

bool launcher_running() { return process_running(L"Bloodborne.exe"); }
bool game_running() { return process_running(L"Play Bloodborne.exe") || process_running(L"bb-probe.exe"); }

bool is_bbport_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / L"Bloodborne.exe", ec) || fs::is_regular_file(dir / L"Play Bloodborne.exe", ec);
}

fs::path find_bbport(const fs::path& exe_dir, const fs::path& chosen, const std::string& game_dir,
                     BbportSource* source) {
    const std::pair<fs::path, BbportSource> candidates[] = {
        {chosen, BbportSource::Chosen},
        {exe_dir.parent_path(), BbportSource::ExeFolder},   // bbbridge lives in a folder inside bbport's
        {exe_dir, BbportSource::ExeFolder},                 // or right next to Bloodborne.exe
        {game_dir.empty() ? fs::path() : fs::path(widen(game_dir)).parent_path(), BbportSource::Launcher}};
    for (const auto& [dir, src] : candidates)
        if (!dir.empty() && is_bbport_dir(dir)) {
            if (source) *source = src;
            return dir;
        }
    if (source) *source = BbportSource::None;
    return {};
}

std::string describe(BbportSource source) {
    switch (source) {
    case BbportSource::Chosen: return "the folder you chose";
    case BbportSource::ExeFolder: return "the folder bbbridge is in";
    case BbportSource::Launcher: return "found from the bbport launcher's game folder";
    default: return "not found";
    }
}

BbportSetup::BbportSetup(const fs::path& exe_dir, const fs::path& bbport_dir) : exe_dir_(exe_dir), chosen_(bbport_dir) {
    reload();
}

void BbportSetup::reload() {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) throw SetupError("APPDATA is not set");
    settings_file.clear();
    for (const wchar_t* name : {L"bbport-launcher", L"bbport-launcher-coop"}) {
        const fs::path p = fs::path(appdata) / name / L"settings.json";
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            settings_file = p;
            break;
        }
    }
    if (settings_file.empty())
        throw SetupError("bbport launcher settings not found. Start the bbport launcher once and close it.");
    settings = read_json(settings_file);
    if (!settings.is_object()) throw SetupError("the bbport launcher's settings.json cannot be read");
    bbport = find_bbport(exe_dir_, chosen_, jstr(settings, "game_dir"), &source);
    if (bbport.empty()) throw SetupError("bbport folder not found - choose the folder with Bloodborne.exe.");
    const std::string user = jstr(settings, "user_dir"), patches = jstr(settings, "patches_dir");
    user_dir = user.empty() ? bbport / L"user" : fs::path(widen(user));
    patches_dir = patches.empty() ? bbport / L"patches" : fs::path(widen(patches));
}

bool BbportSetup::routed() const {
    return jint(settings, "online", 0) && jstr(settings, "online_server") == kBridgeServer &&
           jstr(settings, "online_webapi") == kBridgeWebapi && !jstr(settings, "online_password").empty();
}

bool BbportSetup::overrides_written() const { return ours(overrides_file()); }

// bbport's run.py uses host_overrides.custom.json, when there is one, instead of
// its own addresses: ours sends the game's server traffic to bbbridge, so it may
// only be there while bbbridge runs - else the game cannot reach its server
// ("Failed to acquire Bloodborne server information").
void BbportSetup::set_overrides(bool on) {
    std::error_code ec;
    if (on) {
        fs::create_directories(user_dir, ec);
        if (fs::exists(overrides_file(), ec) && !ours(overrides_file())) backup(overrides_file());
        write_json(overrides_file(), overrides());
    } else if (ours(overrides_file())) {
        fs::remove(overrides_file(), ec);
        const fs::path prev = overrides_file().wstring() + widen(kBackup);
        if (fs::exists(prev, ec)) fs::rename(prev, overrides_file(), ec);
    }
}

bool BbportSetup::game_ready() const {
    const std::string dir = jstr(settings, "game_dir");
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(fs::path(widen(dir)) / L"eboot.bin", ec);
}

bool BbportSetup::patched() const {
    std::error_code ec;
    return fs::is_regular_file(patch_file(), ec);
}

void BbportSetup::save_settings() {
    backup(settings_file);
    write_json(settings_file, settings);
}

void BbportSetup::set_npid(const std::string& npid) {
    settings["online_npid"] = npid;
    save_settings();
}

bool BbportSetup::shadnet_account_set() const {
    const std::string pw = jstr(settings, "online_password");
    return !npid().empty() && !pw.empty() && pw != kPlaceholderPassword;
}

void BbportSetup::enable() {
    // On bbport's own server now: keep that setup (server, shadNet account) for disable(),
    // also when the player changed it since the last time.
    if (jstr(settings, "online_server") != kBridgeServer)
        write_json(settings_file.wstring() + widen(kBackup), settings);
    settings["online"] = true;
    settings["online_server"] = kBridgeServer;
    settings["online_webapi"] = kBridgeWebapi;
    // bbport does not go online without a password (np_handler.cpp: "shadNet
    // enabled but credentials missing, skipping"). The bridge never reads it; a
    // player without a shadNet account just needs something there.
    if (jstr(settings, "online_password").empty()) settings["online_password"] = kPlaceholderPassword;
    const std::string port = std::to_string(kP2pPort);
    settings["extra_env"] = merge_env(jstr(settings, "extra_env"), kP2pVar, &port);
    save_settings();
    set_overrides(true);
}

void BbportSetup::disable() {
    const ojson old = read_json(settings_file.wstring() + widen(kBackup));
    std::string server = jstr(old, "online_server", kPublicServer);
    if (server.empty() || server == kBridgeServer) server = kPublicServer;
    std::string webapi = jstr(old, "online_webapi", "");
    if (webapi == kBridgeWebapi) webapi.clear();
    settings["online_server"] = server;
    settings["online_webapi"] = webapi;
    if (jhas(old, "online_npid")) settings["online_npid"] = jstr(old, "online_npid");   // the shadNet account, not The Hunter's Dream's
    if (jstr(settings, "online_password") == kPlaceholderPassword) {   // ours: back to what it was
        const std::string before = jstr(old, "online_password");
        settings["online_password"] = before == kPlaceholderPassword ? "" : before;
    }
    settings["extra_env"] = merge_env(jstr(settings, "extra_env"), kP2pVar, nullptr);
    save_settings();
    set_overrides(false);
}

void BbportSetup::set_patch(bool on) {
    std::error_code ec;
    if (on) {
        fs::create_directories(patches_dir, ec);
        std::ofstream(patch_file(), std::ios::trunc) << kPatchXml;
    } else {
        fs::remove(patch_file(), ec);
    }
}

}  // namespace bb
