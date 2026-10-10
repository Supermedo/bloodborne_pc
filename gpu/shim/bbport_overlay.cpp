// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The menu's text in the chosen language (BbSettings::MenuText: menu_language in bbport.ini).
const char* T(const char* english, const char* portuguese, const char* russian) {
    return BbSettings::MenuText(english, portuguese, russian);
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    // "###bbport_menu": the window keeps its ID (position, size) when the language changes.
    char title[128];
    std::snprintf(title, sizeof(title), "%s  (Insert / L3+R3)###bbport_menu",
                  T("Bloodborne — settings", "Bloodborne — configurações", "Bloodborne — настройки"));
    if (!ImGui::Begin(title, &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f %s)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg, T("ms", "ms", "мс"));

    static const char* languages[] = {"English", "Português", "Русский"};
    int language = s.menu_language;
    if (ImGui::Combo("Language / Idioma / Язык", &language, languages, 3)) {
        Store(s.menu_language, language, true);
    }

    ImGui::SeparatorText(T("Temporal upscaler", "Upscaler temporal", "Временной апскейлер"));
    const char* upscalers[] = {T("Off", "Desligado", "Выкл"), "FSR 3.1", "FSR 4 (INT8)",
                               "FSR 4.1.1 (INT8)",
                               T("TAA (native anti-aliasing)", "TAA (antisserrilhado nativo)",
                                 "TAA (нативное сглаживание)"),
                               "DLSS (NVIDIA RTX)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(T("Upscaler", "Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("— not supported by the graphics card",
                                            "— não suportado pela placa de vídeo",
                                            "— не поддерживается видеокартой"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("— in progress", "— em desenvolvimento", "— в работе"));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s %s",
                           T("FSR 4 unavailable:", "FSR 4 indisponível:", "FSR 4 недоступен:"),
                           problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(T("The mode chosen above is active. FSR 4 can be chosen again.",
                                     "O modo escolhido acima está ativo. Dá para escolher o FSR 4 "
                                     "de novo.",
                                     "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(T("FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, replayed in Vulkan "
                   "(same result as the DLL). One model for Native..Performance and another for "
                   "Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (needs the DLL and "
                   "Proton).",
                   "FSR 4.1.1 em modo INT8: o modelo da DLL 4.1.1 da AMD, reproduzido em Vulkan "
                   "(mesmo resultado da DLL). Um modelo para Native..Performance e outro para "
                   "Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (precisa da DLL e do "
                   "Proton).",
                   "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                   "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                   "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                   "Proton)."));
        } else {
            Hint(T("FSR 4 in INT8 mode (model v07 from the AMD FidelityFX SDK sources). Better "
                   "quality than FSR 3.1, but a heavier pass. Changing the preset rebuilds the "
                   "model (short pause). Assets: tools/fetch_fsr4_assets.sh.",
                   "FSR 4 em modo INT8 (modelo v07 do código do AMD FidelityFX SDK). Qualidade "
                   "melhor que o FSR 3.1, mas mais pesado. Trocar o preset reconstrói o modelo "
                   "(pausa curta). Assets: tools/fetch_fsr4_assets.sh.",
                   "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество "
                   "выше, чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель "
                   "(короткая пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        Checkbox(T("FSR 4: auto exposure", "FSR 4: exposição automática", "FSR 4: авто-экспозиция"),
                 s.fsr4_auto_exposure);
        Checkbox(T("FSR 4: invert jitter sign", "FSR 4: inverter sinal do jitter",
                   "FSR 4: обратный знак jitter"),
                 s.fsr4_invert_jitter);
        Hint(T("For ghosting checks: the FSR 4 network normalizes color by exposure and uses it "
               "to decide when to drop past frames. Applied at once, no restart.",
               "Para testar rastros (ghosting): a rede do FSR 4 normaliza a cor pela exposição e "
               "decide por ela quando descartar quadros anteriores. Aplica na hora, sem reiniciar.",
               "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
               "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(T("Preset", "Preset", "Пресет"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[96];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, %s %dx%d)",
                          BbSettings::PresetName(i), scale, T("render", "render", "рендер"),
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", T("TAA smooths the scene at the output resolution, without the "
                                   "FSR model or upscaling. The saved FSR preset comes back when "
                                   "FSR is chosen.",
                                   "O TAA suaviza a cena na resolução de saída, sem o modelo do FSR "
                                   "e sem upscaling. O preset do FSR salvo volta quando o FSR for "
                                   "escolhido.",
                                   "TAA сглаживает сцену в разрешении вывода, без модели FSR и "
                                   "апскейлинга. Сохранённый пресет FSR восстановится при выборе "
                                   "FSR."));
    }
    ImGui::Text("%s: %d x %d", T("Active scene render", "Render da cena ativo", "Активный рендер сцены"),
                s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("%s: %s", T("Preset at start", "Preset na abertura", "Пресет при запуске"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(T("With an output other than 1080p the whole game is drawn at the preset's "
                   "resolution (a patch at start): fastest on the Steam Deck and weaker GPUs. "
                   "Preset or output changes need a restart. \"Live resolution changes\" below "
                   "allows changes without a restart (post-processing then stays at 1080p, "
                   "slower).",
                   "Com saída diferente de 1080p o jogo todo é desenhado na resolução do preset "
                   "(patch na abertura): o mais rápido no Steam Deck e em GPUs mais fracas. Mudar "
                   "o preset ou a saída exige reiniciar. \"Mudança de resolução ao vivo\" abaixo "
                   "permite mudar sem reiniciar (o pós-processamento fica em 1080p, mais lento).",
                   "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при "
                   "запуске): это быстрее всего на Steam Deck и слабых GPU. Смена пресета или "
                   "разрешения вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже "
                   "включает смену без перезапуска (постобработка тогда остаётся в 1080p, "
                   "медленнее)."));
        } else {
            Hint(T("BB_RENDER_RES fixes the scene size at start. Remove this explicit variable to "
                   "change resolution and presets without restarting the game.",
                   "BB_RENDER_RES fixa o tamanho da cena na abertura. Remova essa variável para "
                   "mudar resolução e presets sem reiniciar o jogo.",
                   "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную "
                   "переменную для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(T("Native AA: FSR works as anti-aliasing. The other presets lower the scene's render "
               "resolution relative to the output. The interface is drawn at the output "
               "resolution. The preset applies from the next frame, no restart.",
               "Native AA: o FSR funciona como antisserrilhado. Os outros presets reduzem a "
               "resolução da cena em relação à saída. A interface é desenhada na resolução de "
               "saída. O preset vale a partir do próximo quadro, sem reiniciar.",
               "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение "
               "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
               "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(T("Sharpening (RCAS)", "Nitidez (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(T("Sharpness", "Força da nitidez", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(T("Up to 1: the upscaler's own sharpening (RCAS). Above 1 one more RCAS pass is added. "
           "Ctrl+click the slider to type an exact value.",
           "Até 1: a nitidez do próprio upscaler (RCAS). Acima de 1 entra mais uma passada de "
           "RCAS. Ctrl+clique no controle para digitar um valor exato.",
           "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
           "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    Checkbox(T("Subpixel jitter", "Jitter de subpixel", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(T("Each frame the scene moves by a fraction of a pixel, and the upscaler gathers more "
           "detail from several frames. Without it there is only history anti-aliasing.",
           "A cada quadro a cena se desloca uma fração de pixel, e o upscaler junta mais "
           "detalhes de vários quadros. Sem isso, sobra só o antisserrilhado pelo histórico.",
           "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
           "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(T("Reactive mask", "Máscara reativa", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(T("Enable mask", "Ativar máscara", "Включить маску"), s.reactive);
    Hint(T("Marks transparent effects (particles, haze) so the upscaler relies less on past "
           "frames. Fewer trails behind effects, but jitter comes back under them.",
           "Marca efeitos transparentes (partículas, névoa) para o upscaler depender menos dos "
           "quadros anteriores. Menos rastros atrás dos efeitos, mas o tremido volta embaixo deles.",
           "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше опирался на "
           "прошлые кадры. Меньше шлейфов за эффектами, но под ними возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(T("Scale", "Escala", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(T("Threshold", "Limiar", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(T("Maximum", "Máximo", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(T("Show mask (debug)", "Mostrar máscara (depuração)",
                          "Показать маску (отладка)"),
                        &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(T("Character motion vectors", "Vetores de movimento dos personagens",
               "Векторы движения персонажей"),
             s.object_motion);
    Hint(T("Exact vectors for animated objects: clothes and weapons break up less in motion. The "
           "static scene gets no extra pass. Applies after restarting the game.",
           "Vetores exatos para objetos animados: roupas e armas se desmancham menos em "
           "movimento. A cena parada não ganha passada extra. Aplica depois de reiniciar o jogo.",
           "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
           "при движении. Статичная сцена не получает дополнительный проход. "
           "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(T("Show motion vectors (debug)", "Mostrar vetores de movimento (depuração)",
                          "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(T("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel "
           "got an exact object vector, not just camera motion. A moving object with no blue and "
           "no red/green is treated as still by the upscaler, hence trails.",
           "Vermelho/verde: movimento horizontal/vertical (8 pixels = brilho total). Azul: o "
           "pixel recebeu um vetor exato do objeto, não só o movimento da câmera. Um objeto em "
           "movimento sem azul e sem vermelho/verde é tratado como parado pelo upscaler, daí o "
           "rastro.",
           "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
           "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
           "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
           "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(T("Output resolution", "Resolução de saída", "Разрешение вывода"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(T("Output resolution", "Resolução de saída", "Разрешение вывода"),
                          outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(T("Size of the finished frame and interface. The preset sets the scene size relative "
               "to the output: 4K Performance = 1920x1080. Applies after restarting the game.",
               "Tamanho do quadro final e da interface. O preset define o tamanho da cena em "
               "relação à saída: 4K Performance = 1920x1080. Aplica depois de reiniciar o jogo.",
               "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
               "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
    } else {
        Hint(T("The size of the finished frame and interface changes at the next frame "
               "boundary. The preset sets the scene size relative to the output: 4K Performance "
               "= 1920x1080. Changing it resets FSR's history and may cause a short pause.",
               "O tamanho do quadro final e da interface muda no próximo quadro. O preset define "
               "o tamanho da cena em relação à saída: 4K Performance = 1920x1080. Mudar zera o "
               "histórico do FSR e pode causar uma pausa curta.",
               "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
               "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
               "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
    }
    const char* live_modes[] = {T("Auto (by graphics card)", "Automático (pela placa de vídeo)",
                                  "Авто (по видеокарте)"),
                                T("Off (faster)", "Desligada (mais rápido)", "Выключена (быстрее)"),
                                T("On", "Ligada", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(T("Live resolution changes", "Mudança de resolução ao vivo",
                            "Смена разрешения на лету"),
                          live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(T("On: output resolution and preset change without a restart, but the game's "
           "post-processing stays at 1080p - noticeably slower on the Steam Deck and older "
           "graphics cards. Off: everything is drawn at the preset's resolution, changes need a "
           "restart. Auto turns it on for strong discrete graphics cards. Applies after "
           "restarting the game.",
           "Ligada: resolução de saída e preset mudam sem reiniciar, mas o pós-processamento do "
           "jogo fica em 1080p - bem mais lento no Steam Deck e em placas antigas. Desligada: "
           "tudo é desenhado na resolução do preset, e mudar exige reiniciar. Automático liga em "
           "placas dedicadas fortes. Aplica depois de reiniciar o jogo.",
           "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка "
           "игры остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
           "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто "
           "включает её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    ImGui::SeparatorText(T("Game effects (after restart)", "Efeitos do jogo (após reiniciar)",
                           "Эффекты игры (после перезапуска)"));
    const char* lods[] = {T("Highest (-2)", "Máxima (-2)", "Максимальная (-2)"),
                          T("As in the game", "Como no jogo", "Как в игре"),
                          T("Lower (1)", "Menor (1)", "Ниже (1)"),
                          T("Lowest (2)", "Mínima (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(T("Model detail", "Detalhe dos modelos", "Детализация моделей"),
                          lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        Checkbox(BbSettings::EffectLabel(e), s.effects[e]);
    }
    Hint(T("Effects are switched by game patches at start (patches/Bloodborne.xml). Motion blur "
           "and shadows from dynamic lights put noticeable load on the GPU.",
           "Os efeitos são ligados e desligados por patches do jogo na abertura "
           "(patches/Bloodborne.xml). Desfoque de movimento e sombras de luzes dinâmicas pesam "
           "bastante na GPU.",
           "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
           "Размытие в движении и тени от динамических источников заметно нагружают GPU."));
    Hint(T("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left "
           "touchpad / Tab. Needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from "
           "Nexus mod #253. Right touchpad: Backspace.",
           "Câmera livre: segure Cross e aperte L3 (teclado: Espaço + Z). Debug menu: touchpad "
           "esquerdo / Tab. Precisa de DbgFont14h.ccm e DbgFont14h.tpf em dvdroot_ps4/font, do "
           "mod #253 do Nexus. Touchpad direito: Backspace.",
           "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
           "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
           "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."));

    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           T("Changes apply after restarting the game",
                             "As mudanças valem depois de reiniciar o jogo",
                             "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(T("Apply and restart the game", "Aplicar e reiniciar o jogo",
                            "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(T("Other", "Outros", "Прочее"));
    Checkbox(T("FPS counter in the corner", "Contador de FPS no canto", "Счётчик FPS в углу"),
             s.show_fps);

    ImGui::Spacing();
    if (ImGui::Button(T("Close", "Fechar", "Закрыть"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("Settings are saved in bbport.ini", "As configurações ficam em bbport.ini",
                                "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS  %.1f %s  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg, BbSettings::MenuText("ms", "ms", "мс"),
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                                                         : "");
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || BbSettings::Get().show_fps);
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
