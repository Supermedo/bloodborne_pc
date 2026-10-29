# SPDX-License-Identifier: GPL-3.0-or-later
"""Version labels share the existing translations; method names stay unambiguous."""
import bbport_lang

HINT = 'FSR 4.1.1 Vulkan uses fsr4vk; FSR 4.1.2 uses AMD Vulkan. AMD DX12 is available for both versions. Switch methods live in the in-game overlay; a short pause is possible.'
TEXTS = {
 'de': ('4.1.1: Vulkan über fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 für beide Versionen. Im Overlay live wechseln; kurze Pause möglich.', 'Unterstützung wird geprüft…', 'AMD-Grafikkarte erforderlich.', 'AMD-Treiber aktualisieren.'),
 'ru': ('4.1.1: Vulkan через fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 для обеих версий. Смена метода на лету в игровом меню; возможна короткая пауза.', 'Проверка поддержки…', 'Нужна видеокарта AMD.', 'Обновите драйвер AMD.'),
 'ar': ('4.1.1: ‏Vulkan عبر fsr4vk. ‏4.1.2: ‏AMD Vulkan. ‏AMD DX12 لكلا الإصدارين. يمكن التبديل مباشرة في قائمة اللعبة؛ قد يحدث توقف قصير.', 'جارٍ التحقق من الدعم…', 'يلزم معالج رسومات AMD.', 'حدّث برنامج تشغيل AMD.'),
 'es': ('4.1.1: Vulkan mediante fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 para ambas versiones. Cambia el método en el menú del juego; puede haber una breve pausa.', 'Comprobando compatibilidad…', 'Se requiere una GPU AMD.', 'Actualiza el controlador AMD.'),
 'pt': ('4.1.1: Vulkan via fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 para ambas as versões. Troque o método no menu do jogo; pode haver uma breve pausa.', 'Verificando suporte…', 'Requer GPU AMD.', 'Atualize o driver AMD.'),
 'fr': ('4.1.1 : Vulkan via fsr4vk. 4.1.2 : AMD Vulkan. AMD DX12 pour les deux versions. Changement direct dans le menu du jeu ; courte pause possible.', 'Vérification de la compatibilité…', 'Carte graphique AMD requise.', 'Mettez à jour le pilote AMD.'),
 'it': ('4.1.1: Vulkan tramite fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 per entrambe le versioni. Cambio diretto nel menu di gioco; possibile breve pausa.', 'Verifica del supporto…', 'Richiede una GPU AMD.', 'Aggiorna il driver AMD.'),
 'pl': ('4.1.1: Vulkan przez fsr4vk. 4.1.2: AMD Vulkan. AMD DX12 dla obu wersji. Zmiana na żywo w menu gry; możliwa krótka pauza.', 'Sprawdzanie obsługi…', 'Wymagana karta AMD.', 'Zaktualizuj sterownik AMD.'),
 'tr': ('4.1.1: fsr4vk ile Vulkan. 4.1.2: AMD Vulkan. Her iki sürümde AMD DX12. Oyun menüsünde anında değiştirin; kısa bir duraklama olabilir.', 'Destek denetleniyor…', 'AMD GPU gerekir.', 'AMD sürücüsünü güncelleyin.'),
 'zh': ('4.1.1：通过 fsr4vk 使用 Vulkan。4.1.2：AMD Vulkan。两个版本均可使用 AMD DX12。可在游戏内菜单中实时切换，可能会短暂停顿。', '正在检查支持…', '需要 AMD 显卡。', '请更新 AMD 驱动程序。'),
 'ja': ('4.1.1：fsr4vk 経由の Vulkan。4.1.2：AMD Vulkan。両バージョンで AMD DX12 を使用可能。ゲーム内メニューで即時切替。短い停止が発生する場合があります。', '対応状況を確認中…', 'AMD GPU が必要です。', 'AMD ドライバーを更新してください。'),
 'ko': ('4.1.1: fsr4vk를 통한 Vulkan. 4.1.2: AMD Vulkan. 두 버전 모두 AMD DX12 지원. 게임 내 메뉴에서 바로 변경할 수 있으며 잠시 멈출 수 있습니다.', '지원 확인 중…', 'AMD GPU가 필요합니다.', 'AMD 드라이버를 업데이트하세요.'),
}
for language, texts in TEXTS.items():
    table = bbport_lang.table(language)
    old = 'FSR 4.1.1 (experimental)'
    translated = table.get(old, 'FSR 4.1.1 (экспериментально)' if language=='ru' else old)
    table['FSR 4.1.2 (experimental)'] = translated.replace('4.1.1','4.1.2')
    old = 'FSR 4.1.1 backend'
    table['FSR 4.1 backend'] = table.get(old, 'Метод FSR 4.1.1' if language=='ru' else old).replace('4.1.1','4.1')
    table.update({HINT: texts[0], 'FSR 4.1.2: checking support…': 'FSR 4.1.2: '+texts[1],
        'FSR 4.1.2 requires an AMD GPU.': 'FSR 4.1.2: '+texts[2],
        'FSR 4.1.2: update the AMD driver.': 'FSR 4.1.2: '+texts[3]})
