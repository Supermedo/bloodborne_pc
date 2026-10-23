// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

int main() {
    using namespace BbSettings;
    wchar_t path[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"bbf", 0, path));
    _wputenv_s(L"BB_CONFIG", path);
    _putenv_s("BB_FRAME_GEN", "");
    auto& s = Get();
    for (const int choice : {FrameGenFsr401, FrameGenFsr402}) {
        SelectFrameGen(choice);
        ConfigureFg401Support(true, nullptr, choice);
        assert(s.MlFgSupported(choice));
        FrameGenFailure(choice, "ML provider unavailable.");
        assert(s.frame_gen == choice && s.ActiveFrameGen() == FrameGenFsr3);
        Save(); Load();
        assert(s.frame_gen == choice && s.ActiveFrameGen() == choice);
        FrameGenFailure(choice, "ML runtime failed.");
        const int other = choice == FrameGenFsr401 ? FrameGenFsr402 : FrameGenFsr401;
        SelectFrameGen(other);
        assert(s.ActiveFrameGen() == other); // Failure belongs to one provider only.
        FrameGenFailure(other, "Other provider failed.");
        FrameGenFailure(FrameGenFsr3, "Fallback failed.");
        assert(s.frame_gen == other && s.ActiveFrameGen() == FrameGenOff);
        Save(); Load();
        assert(s.frame_gen == other && s.ActiveFrameGen() == other);
    }
    SelectFrameGen(FrameGenFsr401);
    ConfigureFg401Support(true, nullptr);
    assert(s.ActiveFrameGen() == FrameGenFsr401);
    for (const char* reason : {"ML-FG file missing: test.dll", "ML-FG support check failed.",
                              "ML-FG requires Windows 11.", "ML-FG context creation failed."}) {
        SelectFrameGen(FrameGenFsr401);
        FrameGenFailure(FrameGenFsr401, reason);
        assert(s.frame_gen == FrameGenFsr401 && s.ActiveFrameGen() == FrameGenFsr3);
        assert(std::string(s.fg401_problem.load()) == reason);
        assert(std::string(s.frame_gen_problem.load()).find("FSR 3.1 FG active") != std::string::npos);
        Save();
        std::ifstream file{std::filesystem::path(path)};
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        assert(text.find("frame_gen=fsr401\n") != std::string::npos);
        Load();
        assert(s.frame_gen == FrameGenFsr401 && s.ActiveFrameGen() == FrameGenFsr401);
        assert(!s.frame_gen_problem.load());
    }
    FrameGenFailure(FrameGenFsr401, "ML-FG failed.");
    FrameGenFailure(FrameGenFsr3, "FSR 3.1 failed too.");
    assert(s.frame_gen == FrameGenFsr401 && s.ActiveFrameGen() == FrameGenOff);
    Save(); Load();
    assert(s.frame_gen == FrameGenFsr401 && s.ActiveFrameGen() == FrameGenFsr401);
    SelectFrameGen(FrameGenFsr3);
    assert(s.ActiveFrameGen() == FrameGenFsr3 && !s.frame_gen_problem.load());
    Save(); Load();
    assert(s.frame_gen == FrameGenFsr3);
    SelectFrameGen(FrameGenOff);
    Save(); Load();
    assert(s.frame_gen == FrameGenOff && s.ActiveFrameGen() == FrameGenOff);
    SelectFrameGen(FrameGenDlss);
    FrameGenFailure(FrameGenDlss, "DLSS failed.");
    assert(s.frame_gen == FrameGenFsr3); // Existing non-ML fallback policy.
    std::filesystem::remove(path);
    std::puts("ML-FG preference survives probe/runtime/fallback failures and saves; explicit choices and restart PASS");
}
