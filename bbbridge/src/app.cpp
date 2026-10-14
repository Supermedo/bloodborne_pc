// bbbridge.exe: the bridge with a window. It sits in a folder inside bbport's
// (E:\bloodborne\bbbridge\), keeps its config, account and log there, points
// bbport at itself, and starts the game.
#include "bridge.hpp"   // first: winsock2.h before anything pulls in windows.h's winsock.h

#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>

#include "account_link.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "setup.hpp"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

using namespace bb;

// ---- D3D11 boilerplate (from Dear ImGui's example_win32_directx11) ----
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_close_requested = false;

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void release_rtv() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no GPU driver: the software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

void destroy_device() {
    release_rtv();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        g_close_requested = true;   // the app decides (the game may still be running)
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the app ----

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// "" when started, else why not
std::string launch(const fs::path& exe, const fs::path& cwd) {
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) return narrow(exe.wstring()) + " is missing.";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, err, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        std::string why = text ? narrow(text) : fmt("error %lu", err);
        if (text) LocalFree(text);
        while (!why.empty() && (why.back() == '\n' || why.back() == '\r' || why.back() == '.')) why.pop_back();
        return "Could not start " + narrow(exe.wstring()) + ": " + why + ".";
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return "";
}

// A two-way switch: "left" label, the switch, "right" label; *right is the
// state. The switch and either label change it. True when changed.
bool network_switch(const char* id, const char* left, const char* right, bool* on) {
    const ImVec4 lit(0.95f, 0.95f, 0.95f, 1), dim(0.5f, 0.5f, 0.5f, 1);
    const ImVec4 left_track(0.22f, 0.42f, 0.62f, 1), right_track(0.62f, 0.16f, 0.16f, 1);
    bool changed = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(*on ? dim : lit, "%s", left);
    if (ImGui::IsItemClicked() && *on) *on = false, changed = true;
    ImGui::SameLine();
    const float h = ImGui::GetFrameHeight(), w = h * 2.1f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("switch", ImVec2(w, h))) *on = !*on, changed = true;
    const bool hovered = ImGui::IsItemHovered();
    // slide the knob over ~0.12 s
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("t");
    float t = store->GetFloat(key, *on ? 1.0f : 0.0f);
    const float step = ImGui::GetIO().DeltaTime / 0.12f;
    t = *on ? (t + step > 1 ? 1 : t + step) : (t - step < 0 ? 0 : t - step);
    store->SetFloat(key, t);
    ImVec4 track(left_track.x + (right_track.x - left_track.x) * t, left_track.y + (right_track.y - left_track.y) * t,
                 left_track.z + (right_track.z - left_track.z) * t, 1);
    if (hovered) track = ImVec4(track.x * 1.15f, track.y * 1.15f, track.z * 1.15f, 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(track), h * 0.5f);
    const float r = h * 0.5f - 3;
    dl->AddCircleFilled(ImVec2(p.x + r + 3 + t * (w - 2 * r - 6), p.y + h * 0.5f), r, ImGui::GetColorU32(lit));
    ImGui::SameLine();
    ImGui::TextColored(*on ? lit : dim, "%s", right);
    if (ImGui::IsItemClicked() && !*on) *on = true, changed = true;
    ImGui::PopID();
    return changed;
}

std::optional<fs::path> pick_folder(HWND owner) {
    std::optional<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS);
    dlg->SetTitle(L"bbport folder (the one with Bloodborne.exe)");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

class App {
public:
    explicit App(HWND hwnd) : hwnd_(hwnd), dir_(exe_dir()) {
        set_log_file((dir_ / L"bbbridge.log").wstring());
        set_log_sink([this](const std::string& line) {
            std::lock_guard<std::mutex> lk(log_mu_);
            lines_.push_back(line);
            while (lines_.size() > 3000) lines_.pop_front();
            scroll_ = true;
        });
        load_config();
        reload_setup();
        log(fmt("bbbridge %s - bbport: %s (%s)", kVersion, bbport_dir_.empty() ? "-" : narrow(bbport_dir_.wstring()).c_str(),
                describe(bbport_source_).c_str()));
        if (!setup_error_.empty()) log(setup_error_);
        check_folder_on_start();
        if (setup_ && setup_->routed()) start_bridge();   // routed already: be ready when the game starts
    }

    ~App() {
        stop_bridge();   // and the game's server traffic goes back to bbport's own addresses
    }

    void frame();
    bool wants_close() {
        if (!g_close_requested) return false;
        if (game_running_ && bridge_) {
            confirm_close_ = true;
            g_close_requested = false;
            return false;
        }
        return true;
    }

private:
    void load_config();
    void save_config();
    void try_setup();
    void reload_setup();
    void refresh();
    void check_folder_on_start();
    void choose_folder();
    void start_bridge();
    void stop_bridge();
    void play();
    void open_launcher();
    void fail(const std::string& message);
    void status_panel();
    void setup_panel();
    void account_row();
    void account_popups();
    void message_popups();
    void log_panel();
    void use_npid(const std::string& npid);
    void apply_defaults();
    void sync_overrides();
    bool live_map_wanted() const { return jint(config_, "live_map", 1) != 0; }
    bool route_wanted() const { return jint(config_, "route", 1) != 0; }   // the box, as last ticked; bbhost by default
    bool signed_in() const { return !account_.empty() && !account_kind(state_path(), account_).empty(); }
    std::wstring state_path() const { return (dir_ / L"bbbridge_state.json").wstring(); }
    std::string auth_server() const { return jstr(config_, "auth_server", "https://thehuntersdream.com"); }

    HWND hwnd_;
    fs::path dir_;
    json config_ = json::object();
    std::unique_ptr<Bridge> bridge_;
    std::optional<BbportSetup> setup_;
    std::string setup_error_;
    fs::path chosen_bbport_;
    fs::path bbport_dir_;   // known even when setup_ is not (no launcher settings yet)
    BbportSource bbport_source_ = BbportSource::None;
    // The Hunter's Dream account (bbbridge_config.json "account"). It is bbport's
    // online id only while routed: unrouted, bbport keeps its own shadNet account.
    std::string account_;
    bool launcher_running_ = false, game_running_ = false, routed_ = false, patched_ = false;
    double last_refresh_ = 0;
    std::string error_, folder_question_;
    bool open_error_ = false, open_folder_ = false;
    bool confirm_close_ = false;
    std::unique_ptr<DiscordLink> link_;
    bool open_link_ = false, open_web_ = false;
    std::string web_code_, web_error_;
    std::mutex log_mu_;
    std::deque<std::string> lines_;
    bool scroll_ = false;
};

void App::load_config() {
    std::ifstream in(dir_ / L"bbbridge_config.json");
    if (in) {
        json j = json::parse(in, nullptr, false);
        if (j.is_object()) config_ = j;
    }
    if (jhas(config_, "bbport_dir")) chosen_bbport_ = fs::path(widen(jstr(config_, "bbport_dir")));
    account_ = jstr(config_, "account");
}

void App::save_config() {
    std::ofstream(dir_ / L"bbbridge_config.json", std::ios::trunc) << config_.dump(2) << "\n";
}

void App::try_setup() {
    try {
        setup_.emplace(dir_, chosen_bbport_);
        setup_error_.clear();
        bbport_dir_ = setup_->bbport;
        bbport_source_ = setup_->source;
        // older bbbridge kept the account only as bbport's online id
        if (account_.empty() && !account_kind(state_path(), setup_->npid()).empty()) {
            account_ = setup_->npid();
            config_["account"] = account_;
            save_config();
        }
    } catch (const std::exception& e) {
        setup_.reset();
        setup_error_ = e.what();
        bbport_dir_ = find_bbport(dir_, chosen_bbport_, "", &bbport_source_);   // still lets "bbport settings..." work
    }
}

void App::reload_setup() {
    try_setup();
    refresh();
}

// The first look: say where bbport is unless bbbridge sits inside it (the
// normal install), and ask when it had to guess or a chosen folder is gone.
void App::check_folder_on_start() {
    const std::string here = narrow(bbport_dir_.wstring());
    if (bbport_source_ == BbportSource::None) {
        folder_question_ = "bbport was not found.\n\nPut the bbbridge folder inside bbport's folder (the one with "
                           "Bloodborne.exe), e.g. bloodborne\\bbbridge\\bbbridge.exe, or choose the bbport folder.";
    } else if (!chosen_bbport_.empty() && bbport_source_ != BbportSource::Chosen) {
        folder_question_ = "The bbport folder you chose before has no Bloodborne.exe any more:\n  " +
                           narrow(chosen_bbport_.wstring()) + "\n\nbbbridge found bbport here instead (" +
                           describe(bbport_source_) + "):\n  " + here + "\n\nUse this folder?";
    } else if (bbport_source_ == BbportSource::Launcher) {
        folder_question_ = "bbbridge is not inside a bbport folder. The bbport launcher points to this one:\n  " + here +
                           "\n\nUse this folder?";
    } else {
        return;
    }
    open_folder_ = true;
}

void App::choose_folder() {
    auto p = pick_folder(hwnd_);
    if (!p) return;
    if (!is_bbport_dir(*p)) {
        fail(narrow(p->wstring()) + " is not a bbport folder: it has no Bloodborne.exe or Play Bloodborne.exe.\n\n"
             "Choose the folder that has Bloodborne.exe in it.");
        return;
    }
    chosen_bbport_ = *p;
    config_["bbport_dir"] = narrow(p->wstring());
    save_config();
    reload_setup();
    log("bbport folder: " + narrow(p->wstring()));
    if (!setup_error_.empty()) fail(setup_error_);
}

void App::fail(const std::string& message) {
    log("error: " + message);
    error_ = message;
    open_error_ = true;
}

void App::refresh() {
    launcher_running_ = launcher_running();
    game_running_ = game_running();
    if (!setup_ && !launcher_running_ && bbport_source_ != BbportSource::None) try_setup();   // e.g. the launcher just made its settings
    if (setup_) {
        try {
            if (!launcher_running_) setup_->reload();   // the launcher may have saved new settings
        } catch (...) {
        }
        routed_ = setup_->routed();
        patched_ = setup_->patched();
        if (!jint(config_, "defaults_applied", 0) && !launcher_running_ && !game_running_ && signed_in())
            apply_defaults();
        sync_overrides();
    }
    last_refresh_ = now();
}

// The game's server traffic goes to bbbridge (host_overrides.custom.json) exactly
// while bbport is switched to The Hunter's Dream and the bridge runs: with
// bbbridge closed, or bbport set back to shadNet in its own launcher, the file
// would leave the game unable to reach its server.
void App::sync_overrides() {
    if (!setup_) return;
    const bool want = setup_->routed() && bridge_;   // live: PLAY routes and starts the bridge in one go
    if (want == setup_->overrides_written()) return;
    try {
        setup_->set_overrides(want);
        log(want ? "the game's server traffic goes through bbbridge"
                 : "the game's server traffic goes to bbport's own addresses again");
    } catch (const std::exception& e) {
        log(std::string("host_overrides.custom.json: ") + e.what());
    }
}

// Once, when there is an account to play with: route bbport through bbbridge
// and add the live map patch, so starting from the bbport launcher works too
// and nobody has to remember them. Switching them off later sticks.
void App::apply_defaults() {
    try {
        if (!routed_) setup_->enable();
        if (setup_->npid() != account_) setup_->set_npid(account_);
        if (live_map_wanted() && !patched_) setup_->set_patch(true);
    } catch (const std::exception& e) {
        fail(std::string("Could not set bbport up: ") + e.what());
        return;
    }
    config_["defaults_applied"] = true;
    config_["route"] = true;
    save_config();
    routed_ = setup_->routed();
    patched_ = setup_->patched();
    log("bbport now uses bbbridge" + std::string(patched_ ? ", with the live map patch" : ""));
    if (routed_) start_bridge();
}

void App::start_bridge() {
    if (bridge_) return;
    Config cfg = Config::from_json(config_);
    cfg.state_file = narrow((dir_ / L"bbbridge_state.json").wstring());   // the account, next to the exe
    auto b = std::make_unique<Bridge>(cfg);
    std::string error;
    if (!b->start(&error)) {
        fail("The bridge could not start: " + error);
        return;
    }
    bridge_ = std::move(b);
    sync_overrides();
}

void App::stop_bridge() {
    if (!bridge_) return;
    bridge_->stop();
    bridge_.reset();
    log("bridge stopped");
    sync_overrides();
}

void App::play() {
    refresh();
    if (!setup_) {
        fail(setup_error_);
        return;
    }
    const bool via_bridge = route_wanted();
    if (via_bridge && !signed_in()) {
        fail("Not signed in. Press 'Sign in with Discord...' first - The Hunter's Dream needs your own account.\n\n"
             "(To play in bbport's own shadNet pool instead, move the Online switch to shadNet.)");
        return;
    }
    if (launcher_running_) {
        fail("Close the bbport launcher first - it would undo bbbridge's settings when it saves.");
        return;
    }
    if (!setup_->game_ready()) {
        fail("bbport has no game folder yet. Press 'bbport settings...', choose your Bloodborne folder "
             "(CUSA03173, the one with eboot.bin), close the launcher, then press PLAY.");
        return;
    }
    try {
        if (via_bridge) {
            if (!routed_) setup_->enable();
            if (setup_->npid() != account_) setup_->set_npid(account_);
            if (live_map_wanted() && !patched_) setup_->set_patch(true);
        } else if (routed_) {
            setup_->disable();   // e.g. the launcher was open when the switch moved
        }
    } catch (const std::exception& e) {
        fail(std::string("Could not set bbport up: ") + e.what());
        return;
    }
    if (!via_bridge && setup_->online() && !setup_->shadnet_account_set()) {
        fail("bbport's online play is on but it has no shadNet account.\n\nRegister one at "
             "https://shadnet.shadps4.net, then press 'bbport settings...' > Online co-op and enter the account "
             "name (not the email) and password. Or move the Online switch to The Hunter's Dream "
             "to play with bbhost players using your Discord sign-in.");
        return;
    }
    if (via_bridge) {
        start_bridge();
        if (!bridge_) return;
    }
    if (const std::string err = launch(setup_->play_exe(), setup_->bbport); !err.empty()) {
        fail(err);
        return;
    }
    log(fmt("starting the game (%s): %s", via_bridge ? "bbhost pool through bbbridge" : "bbport's own shadNet",
            narrow(setup_->play_exe().wstring()).c_str()));
    refresh();
}

void App::open_launcher() {
    refresh();
    if (bbport_dir_.empty()) {
        fail("bbport was not found. Press 'Choose bbport folder...' and pick the folder with Bloodborne.exe.");
        return;
    }
    if (launcher_running_) {
        fail("The bbport launcher is already open.");
        return;
    }
    if (game_running_) {
        fail("The game is running - close it before changing bbport's settings.");
        return;
    }
    if (const std::string err = launch(bbport_dir_ / L"Bloodborne.exe", bbport_dir_); !err.empty()) fail(err);
}

void App::status_panel() {
    const BridgeStatus st = bridge_ ? bridge_->status() : BridgeStatus{};
    const ImVec4 green(0.35f, 0.85f, 0.45f, 1), grey(0.6f, 0.6f, 0.6f, 1), amber(0.95f, 0.75f, 0.3f, 1);
    ImGui::TextColored(bridge_ ? green : grey, bridge_ ? "  Bridge running" : "  Bridge stopped");
    if (bridge_) {
        ImGui::SameLine();
        if (st.relay_port)
            ImGui::TextDisabled("  relay port %d%s", st.relay_port,
                                st.mapped ? (" - seen as " + st.mapped->str()).c_str() : "");
        else
            ImGui::TextDisabled("  waiting for The Hunter's Dream's STUN answer");
    }
    if (bridge_) {
        if (st.logged_in) ImGui::Text("Signed in as %s", st.npid.c_str());
        else if (!st.npid.empty()) ImGui::TextColored(amber, "Signing in as %s ...", st.npid.c_str());
        else ImGui::TextDisabled("Waiting for the game to connect");
        if (st.room_id) ImGui::Text("Room %llu - %d member(s)", static_cast<unsigned long long>(st.room_id), st.room_members);
        if (!st.peers.empty() && ImGui::BeginTable("peers", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Player");
            ImGui::TableSetupColumn("Member");
            ImGui::TableSetupColumn("Packets out");
            ImGui::TableSetupColumn("Packets in");
            ImGui::TableHeadersRow();
            for (const auto& p : st.peers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.online_id.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", p.member_id);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(p.to_peer));
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(p.to_port));
            }
            ImGui::EndTable();
        }
    }
}

void App::setup_panel() {
    ImGui::SeparatorText("Setup");
    if (!bbport_dir_.empty()) {
        ImGui::Text("bbport:");
        ImGui::SameLine();
        ImGui::TextUnformatted(narrow(bbport_dir_.wstring()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", describe(bbport_source_).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Change...")) choose_folder();
    }
    if (!setup_) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", setup_error_.c_str());
        ImGui::PopTextWrapPos();
        if (bbport_dir_.empty() && ImGui::Button("Choose bbport folder...")) choose_folder();
        return;
    }
    if (!setup_->game_ready())
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1),
                           "Choose your game folder (CUSA03173, with eboot.bin) in the bbport launcher first.");
    account_row();
    ImGui::BeginDisabled(launcher_running_);
    bool routed = routed_;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Online:");
    ImGui::SameLine();
    if (network_switch("network", "shadNet", "The Hunter's Dream (bbhost)", &routed)) {
        try {
            if (routed) {
                setup_->enable();
                if (signed_in() && setup_->npid() != account_) setup_->set_npid(account_);
                start_bridge();   // the game's server traffic needs it running
            } else {
                setup_->disable();   // bbport's own shadNet server and account come back
            }
            config_["route"] = routed;
            save_config();
            log(routed ? "bbport now uses bbbridge" : "bbport back on its own shadNet server and account");
        } catch (const std::exception& e) {
            fail(e.what());
        }
        refresh();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Right: bbbridge connects bbport to The Hunter's Dream (bbhost players, Discord sign-in).\n"
                          "Left: bbport's own shadNet server and account, without bbbridge.");
    ImGui::Indent();
    if (routed_)
        ImGui::TextDisabled(signed_in() ? "Co-op with bbhost players through bbbridge, as '%s'."
                                        : "Co-op with bbhost players through bbbridge - sign in with Discord first.",
                            account_.c_str());
    else if (!setup_->online())
        ImGui::TextDisabled("bbport's own settings - online play is off there.");
    else if (setup_->shadnet_account_set())
        ImGui::TextDisabled("bbport's own shadNet pool (%s), as '%s'.", setup_->server().c_str(),
                            setup_->npid().c_str());
    else
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1),
                           "shadNet needs a shadNet account (shadnet.shadps4.net), entered in\n"
                           "bbport settings > Online co-op.");
    ImGui::Unindent();
    bool patched = patched_;
    if (ImGui::Checkbox("Live map patch (show me on thehuntersdream.com's map)", &patched)) {
        setup_->set_patch(patched);
        config_["live_map"] = patched;   // PLAY keeps to it
        save_config();
        refresh();
    }
    ImGui::EndDisabled();
    if (launcher_running_)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "The bbport launcher is open - close it to change these.");
    bool trace = jint(config_, "trace", 0) != 0;
    if (ImGui::Checkbox("Detailed log (for bug reports; applies when the bridge restarts)", &trace)) {
        config_["trace"] = trace;
        save_config();
    }
}

void App::use_npid(const std::string& npid) {
    account_ = npid;
    config_["account"] = npid;
    save_config();
    // bbport's online id changes only while it plays through bbbridge (PLAY sets it too)
    if (!setup_ || !routed_ || launcher_running_) return;
    try {
        setup_->set_npid(npid);
        log(fmt("online id set to '%s'", npid.c_str()));
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

void App::account_row() {
    // Everyone signs in with their own Discord account on The Hunter's Dream;
    // the account's name is bbport's online id (NPID) while routed.
    const std::string kind = account_.empty() ? "" : account_kind(state_path(), account_);
    if (!kind.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.65f, 1.0f, 1), "Signed in as '%s'%s", account_.c_str(),
                           kind == "discord" ? " (Discord)" : " (bbbridge account)");
        ImGui::BeginDisabled(launcher_running_);
        if (ImGui::Button("Website sign-in code")) {
            web_code_ = website_code(auth_server(), state_path(), account_, web_error_);
            open_web_ = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A code for thehuntersdream.com/account, 'Sign in with a code from the game'.");
        ImGui::SameLine();
        if (ImGui::Button("Switch account...")) {
            link_ = std::make_unique<DiscordLink>(auth_server(), state_path());
            link_->start();
            open_link_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Sign out")) {
            const std::string err = account_sign_out(auth_server(), state_path(), account_);
            if (!err.empty()) {
                fail("Sign out failed: " + err);
            } else {
                account_.clear();
                config_.erase("account");
                save_config();
            }
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Not signed in. Sign in with your Discord account to play online.");
    ImGui::BeginDisabled(launcher_running_);
    if (ImGui::Button("Sign in with Discord...", ImVec2(220, 0))) {
        link_ = std::make_unique<DiscordLink>(auth_server(), state_path());
        link_->start();
        open_link_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("opens The Hunter's Dream: sign in there with Discord and approve the code");
}

void App::account_popups() {
    if (open_link_) {
        ImGui::OpenPopup("Sign in with Discord");
        open_link_ = false;
    }
    if (ImGui::BeginPopupModal("Sign in with Discord", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const DiscordLink::State st = link_ ? link_->state() : DiscordLink::State::Idle;
        if (st == DiscordLink::State::Starting) {
            ImGui::TextUnformatted("Asking The Hunter's Dream for a code ...");
        } else if (st == DiscordLink::State::Waiting) {
            ImGui::TextUnformatted("1. Open the sign-in page and sign in with Discord.");
            ImGui::TextUnformatted("2. Enter this code there:");
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1), "   %s", link_->user_code().c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("%s", link_->uri().c_str());
            if (ImGui::Button("Open sign-in page"))
                ShellExecuteW(nullptr, L"open", widen(link_->uri()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button("Copy code")) ImGui::SetClipboardText(link_->user_code().c_str());
            ImGui::TextDisabled("Waiting for the approval ... (%d s left)", static_cast<int>(link_->seconds_left()));
        } else if (st == DiscordLink::State::Approved) {
            ImGui::Text("Signed in as '%s'.", link_->online_id().c_str());
            ImGui::TextUnformatted("bbport will use this account as its online id.");
        } else if (st == DiscordLink::State::Failed) {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "Sign-in failed: %s", link_->message().c_str());
        }
        ImGui::Separator();
        if (st == DiscordLink::State::Approved) {
            if (ImGui::Button("Done")) {
                use_npid(link_->online_id());
                link_.reset();
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (st == DiscordLink::State::Failed && ImGui::Button("Try again")) link_->start();
            if (st == DiscordLink::State::Failed) ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                link_.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (open_web_) {
        ImGui::OpenPopup("Website sign-in code");
        open_web_ = false;
    }
    if (ImGui::BeginPopupModal("Website sign-in code", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (web_code_.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "No code: %s", web_error_.c_str());
        } else {
            ImGui::Text("For thehuntersdream.com/account, 'Sign in with a code from the game' (account '%s'):",
                        account_.c_str());
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.5f, 1), "   %s", web_code_.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("Valid for a few minutes.");
            if (ImGui::Button("Copy code")) ImGui::SetClipboardText(web_code_.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Open the account page"))
                ShellExecuteW(nullptr, L"open", widen(auth_server() + "/account").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::Separator();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::message_popups() {
    const float wrap = ImGui::GetFontSize() * 34;
    if (open_folder_) {
        ImGui::OpenPopup("bbport folder");
        open_folder_ = false;
    }
    if (ImGui::BeginPopupModal("bbport folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextUnformatted(folder_question_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (!bbport_dir_.empty() && ImGui::Button("Use this folder")) {
            if (bbport_source_ == BbportSource::ExeFolder) {   // the normal place: nothing to remember
                chosen_bbport_.clear();
                config_.erase("bbport_dir");
            } else {
                chosen_bbport_ = bbport_dir_;
                config_["bbport_dir"] = narrow(bbport_dir_.wstring());
            }
            save_config();
            reload_setup();
            ImGui::CloseCurrentPopup();
        }
        if (!bbport_dir_.empty()) ImGui::SameLine();
        if (ImGui::Button("Choose another folder...")) {
            ImGui::CloseCurrentPopup();
            choose_folder();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (open_error_) {
        ImGui::OpenPopup("bbbridge###error");
        open_error_ = false;
    }
    if (ImGui::BeginPopupModal("bbbridge###error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.5f, 1), "%s", error_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::log_panel() {
    ImGui::SeparatorText("Log");
    if (ImGui::SmallButton("Copy log")) {
        std::string all;
        {
            std::lock_guard<std::mutex> lk(log_mu_);
            for (const auto& l : lines_) all += l + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) ShellExecuteW(nullptr, L"open", dir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(log_mu_);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(lines_.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(lines_[i].c_str());
        if (scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
        scroll_ = false;
    }
    ImGui::EndChild();
}

void App::frame() {
    if (now() - last_refresh_ > 1.5) refresh();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("bbbridge", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Text("bbbridge %s", kVersion);
    ImGui::SameLine();
    ImGui::TextDisabled("- co-op with bbhost players on The Hunter's Dream");
    ImGui::Spacing();
    status_panel();
    ImGui::Spacing();
    setup_panel();
    ImGui::Spacing();

    // PLAY and bbport settings stay clickable: when something is missing they say what.
    ImGui::BeginDisabled(game_running_);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.16f, 0.16f, 1));
    if (ImGui::Button(game_running_ ? "  PLAYING  " : "    PLAY    ", ImVec2(160, 40))) play();
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(bridge_ ? "Stop bridge" : "Start bridge", ImVec2(130, 40))) {
        if (bridge_) stop_bridge();
        else start_bridge();
    }
    ImGui::SameLine();
    if (ImGui::Button("bbport settings...", ImVec2(160, 40))) open_launcher();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opens the bbport launcher for graphics, mods and cheats. Close it before pressing PLAY here.");
    ImGui::Spacing();
    log_panel();

    account_popups();
    message_popups();
    if (confirm_close_) ImGui::OpenPopup("Close bbbridge?");
    if (ImGui::BeginPopupModal("Close bbbridge?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The game is still running. Closing bbbridge takes it offline.");
        if (ImGui::Button("Close anyway")) {
            confirm_close_ = false;
            g_close_requested = true;
            game_running_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_close_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One bbbridge at a time: a second would fight the first over the ports.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\bbbridge-app");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"bbbridge", nullptr)) SetForegroundWindow(other);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    net_init();
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                   nullptr, nullptr, L"bbbridge", nullptr};
    RegisterClassExW(&wc);
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"bbbridge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(820 * scale), static_cast<int>(760 * scale), nullptr, nullptr, inst, nullptr);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"bbbridge", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14, 12);
    style.FrameRounding = 4;
    style.ScaleAllSizes(scale);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2013, 0x2026, 0};   // Latin-1 + Ext-A
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * scale, nullptr, ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    {
        App app(hwnd);
        bool done = false;
        double last_input = now();
        while (!done) {
            // Full rate (vsync) only while the player is using the window; idle
            // ~10 frames a second (the log and status still move), minimized 4 -
            // the game is the one that should have the CPU and GPU.
            const bool minimized = IsIconic(hwnd) != 0;
            if (now() - last_input > 0.5)
                MsgWaitForMultipleObjects(0, nullptr, FALSE, minimized ? 250 : 100, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
                if (!minimized && msg.message != WM_TIMER && msg.message != WM_PAINT) last_input = now();
            }
            if (done) break;
            if (app.wants_close()) {
                DestroyWindow(hwnd);
                continue;
            }
            if (g_resize_w) {
                release_rtv();
                g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
                g_resize_w = g_resize_h = 0;
                create_rtv();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            if (minimized) continue;   // the app still runs (status, close); nothing to show
            const float clear[4] = {0.07f, 0.07f, 0.08f, 1};
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_swap->Present(1, 0);
        }
    }   // the App stops the bridge here
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
