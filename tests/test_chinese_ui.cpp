// Headless ImGui/CJK tests: no game dump, GPU or Vulkan device required.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "bbport_ui_font.h"

int main() {
    using namespace BbUi;
    assert(WantsChinese("zh") && WantsChinese("zh-CN") && WantsChinese("zh_CN"));
    assert(!WantsChinese("en") && !WantsChinese("ru") && !WantsChinese("zhx"));
    chinese = false;
    assert(std::strcmp(Tr("Close"), "Close") == 0);
    chinese = true;
    assert(std::strcmp(Tr("Close"), "关闭菜单") == 0);
    assert(std::strcmp(Tr("Frame generation"), "帧生成") == 0);
    assert(std::strcmp(Tr("Bloodborne - Settings"), "血源设置") == 0);
    assert(std::strcmp(Tr("Frame times"), "帧耗时") == 0);
    assert(std::strstr(Tr("Quit game###bb_quit"), "###bb_quit") != nullptr);
    assert(std::strcmp(Tr("cheats"), "cheats") == 0); // Internal directory, not a UI label.
    assert(std::strcmp(Tr("##value"), "##value") == 0);
    assert(Tr(nullptr) == nullptr);
    char text[256];
    std::snprintf(text, sizeof(text), Tr("%.0f FPS  %.1f ms"), 60.0, 16.7);
    assert(std::strcmp(text, "60 帧  16.7 毫秒") == 0);

    ImGui::CreateContext();
    auto* atlas = ImGui::GetIO().Fonts;
    ImFontConfig base_font;
    base_font.SizePixels = 18.0f; // Match the overlay's explicit reference size on ImGui 1.92.
    atlas->AddFontDefault(&base_font);
    assert(!ConfigureChineseFont(atlas, "zh", {}));
    assert(!chinese && std::strcmp(Tr("Close"), "Close") == 0);
    const auto system_font = FindChineseFont();
    assert(!ConfigureChineseFont(atlas, "en", system_font));
    assert(!chinese);
    if (!system_font.path.empty()) {
        // Include FSR picker characters beyond the general menu translations.
        const ImWchar extra[] = {0x9f98, 0x9f98, 0};
        assert(ConfigureChineseFont(atlas, "zh", system_font, extra));
        assert(chinese);
        assert(atlas->Build());
        for (ImWchar c : {ImWchar(0x5173), ImWchar(0x95ed), ImWchar(0x6e38), ImWchar(0x620f), ImWchar(0x9f98)}) {
            assert(atlas->Fonts[0]->IsGlyphInFont(c)); // 关闭 / 游戏
        }
        std::puts("CJK system font merge and glyph checks passed");
    } else {
        std::puts("No system CJK font: English fallback passed; glyph checks skipped");
    }
    ImGui::DestroyContext();
    std::puts("Chinese UI tests passed");
}
