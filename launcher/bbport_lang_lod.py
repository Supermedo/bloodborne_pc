# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized warning beside the Model detail (LOD) option: "Highest" (-2) breaks some armor and capes.
Russian is beside the English in bbport_launcher_win.py."""

KEYS = (
    'A game patch (game version 1.09). "Highest" can break armor and capes: parts go missing or look wrong.',
)
TEXTS = {
    'en': KEYS,
    'ar': ('رقعة للعبة (إصدار اللعبة 1.09). قد يُفسد خيار "الأعلى" الدروع والعباءات: تختفي أجزاء منها أو '
           'تظهر بشكل خاطئ.',),
    'es': ('Un parche del juego (versión 1.09). «Máximo» puede romper armaduras y capas: faltan piezas o se ven '
           'mal.',),
    'pt': ('Um patch do jogo (versão 1.09). "Máximo" pode quebrar armaduras e capas: partes somem ou ficam '
           'erradas.',),
    'fr': ('Un patch du jeu (version 1.09). « Maximal » peut casser armures et capes : des pièces manquent ou '
           "s'affichent mal.",),
    'de': ('Ein Spiel-Patch (Spielversion 1.09). „Höchste“ kann Rüstungen und Umhänge kaputt machen: Teile '
           'fehlen oder sehen falsch aus.',),
    'it': ('Una patch del gioco (versione 1.09). "Massimo" può rovinare armature e mantelli: alcune parti '
           'spariscono o appaiono sbagliate.',),
    'pl': ('Łatka gry (wersja 1.09). „Najwyższa” może psuć zbroje i peleryny: części znikają albo wyglądają '
           'źle.',),
    'tr': ('Bir oyun yaması (oyun sürümü 1.09). "En yüksek" zırhları ve pelerinleri bozabilir: parçalar kaybolur '
           'ya da yanlış görünür.',),
    'zh': ('游戏补丁（游戏版本 1.09）。“最高”可能会让护甲和披风出错：部件消失或显示不正确。',),
    'ja': ('ゲームパッチ（ゲームバージョン 1.09）。「最高」では防具やマントが壊れることがあります：パーツが消えたり、'
           '正しく表示されなかったりします。',),
    'ko': ('게임 패치(게임 버전 1.09). "최고"는 갑옷과 망토를 깨뜨릴 수 있습니다: 일부 부위가 사라지거나 잘못 '
           '보입니다.',),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
