// Points an installed bbport (bloodborne_pc for Windows) at the bridge, or back.
// Turning it on keeps bbport's own shadNet setup (server, account) in
// settings.json.before-bbbridge; turning it off puts that back.
// "Routed" means (a backup of every file is kept as *.before-bbbridge):
//  1. %APPDATA%\bbport-launcher\settings.json: online on; co-op server
//     127.0.0.1:31313; WebAPI http://127.0.0.1:31315; extra variable
//     SHADPS4_P2P_PORT=3658 (the bridge must know the port's P2P UDP port).
//  2. <bbport user dir>\host_overrides.custom.json: the game's own server
//     traffic goes through the bridge (the account token, and the play log to
//     The Hunter's Dream instead of FromSoftware's closed upload bucket). Only
//     while bbbridge runs: the app removes it when it closes.
//  3. <bbport>\patches\bbbridge-live-map.xml (optional): play log every 5 s,
//     position every 0.5 s - bbhost's own patch - for the site's live map.
// The bbport launcher must be closed while settings.json changes: it rewrites
// the file when it saves.
#pragma once

#include <filesystem>
#include <string>

#include "util.hpp"

namespace bb {

namespace fs = std::filesystem;

struct SetupError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool process_running(const wchar_t* image);
bool launcher_running();   // Bloodborne.exe
bool game_running();       // Play Bloodborne.exe / bb-probe.exe
bool is_bbport_dir(const fs::path& dir);

// Where bbport was found, in this order: the folder the player chose, the
// folder bbbridge's own folder is in (the normal install), bbbridge's own folder
// (bbbridge.exe next to Bloodborne.exe), the launcher's game folder.
enum class BbportSource { None, Chosen, ExeFolder, Launcher };
fs::path find_bbport(const fs::path& exe_dir, const fs::path& chosen, const std::string& game_dir,
                     BbportSource* source);
std::string describe(BbportSource source);

class BbportSetup {
public:
    // bbport_dir: "" = the folder above this exe, or from the launcher's settings.
    explicit BbportSetup(const fs::path& exe_dir, const fs::path& bbport_dir = {});
    void reload();

    fs::path bbport, settings_file, user_dir, patches_dir;
    BbportSource source = BbportSource::None;
    nlohmann::ordered_json settings;   // the launcher's file, key order kept

    std::string npid() const { return jstr(settings, "online_npid"); }
    std::string server() const { return jstr(settings, "online_server"); }
    bool online() const { return jint(settings, "online", 0) != 0; }
    bool shadnet_account_set() const;   // a name and a real (not bbbridge's placeholder) password
    bool routed() const;                // bbport's settings point at bbbridge
    bool overrides_written() const;     // our host_overrides.custom.json is there
    void set_overrides(bool on);        // only while bbbridge runs (see setup.cpp)
    bool patched() const;
    bool game_ready() const;   // the launcher has a game folder with eboot.bin

    // the caller makes sure the launcher is closed
    void set_npid(const std::string& npid);
    void enable();
    void disable();
    void set_patch(bool on);

    fs::path play_exe() const { return bbport / L"Play Bloodborne.exe"; }
    fs::path launcher_exe() const { return bbport / L"Bloodborne.exe"; }

private:
    void save_settings();
    fs::path overrides_file() const { return user_dir / L"host_overrides.custom.json"; }
    fs::path patch_file() const { return patches_dir / L"bbbridge-live-map.xml"; }
    fs::path exe_dir_, chosen_;
};

}  // namespace bb
