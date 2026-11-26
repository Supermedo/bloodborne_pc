# SPDX-License-Identifier: GPL-2.0-or-later
"""The Borderless fullscreen switch (Picture & performance, after Fullscreen), in every language besides English
and Russian (Russian is in the code). Keyed by the English text like bbport_lang; merged into its tables on
import."""
import bbport_lang

KEYS = [
    'Borderless fullscreen',
    'The game in a window without a frame over the whole monitor: Alt+Tab is instant and other windows can be on '
    'top of it. Turns Fullscreen off.',
]
TEXTS = {
    'ar': [
        'ملء الشاشة بلا إطار',
        'اللعبة في نافذة بلا إطار تغطي الشاشة كلها: يعمل Alt+Tab فورًا ويمكن فتح نوافذ أخرى فوقها. يوقف وضع ملء '
        'الشاشة.',
    ],
    'es': [
        'Pantalla completa sin bordes',
        'El juego en una ventana sin marco que cubre todo el monitor: Alt+Tab es instantáneo y otras ventanas pueden '
        'quedar encima. Desactiva Pantalla completa.',
    ],
    'pt': [
        'Tela cheia sem bordas',
        'O jogo numa janela sem moldura cobrindo o monitor inteiro: o Alt+Tab é instantâneo e outras janelas podem '
        'ficar por cima. Desliga a Tela cheia.',
    ],
    'fr': [
        'Plein écran sans bordure',
        'Le jeu dans une fenêtre sans cadre sur tout l\'écran : Alt+Tab est instantané et d\'autres fenêtres peuvent '
        'passer au-dessus. Désactive le plein écran.',
    ],
    'de': [
        'Randloses Vollbild',
        'Das Spiel in einem Fenster ohne Rahmen über den ganzen Monitor: Alt+Tab wechselt sofort, und andere Fenster '
        'können darüber liegen. Schaltet Vollbild aus.',
    ],
    'it': [
        'Schermo intero senza bordi',
        'Il gioco in una finestra senza cornice su tutto il monitor: Alt+Tab è immediato e altre finestre possono '
        'stare sopra. Disattiva lo schermo intero.',
    ],
    'pl': [
        'Pełny ekran bez ramki',
        'Gra w oknie bez ramki na cały monitor: Alt+Tab działa od razu, a inne okna mogą być nad nią. Wyłącza pełny '
        'ekran.',
    ],
    'tr': [
        'Çerçevesiz tam ekran',
        'Oyun, tüm monitörü kaplayan çerçevesiz bir pencerede: Alt+Tab anında çalışır ve diğer pencereler üstünde '
        'açılabilir. Tam ekranı kapatır.',
    ],
    'zh': [
        '无边框全屏',
        '游戏在覆盖整个显示器的无边框窗口中运行：Alt+Tab 立即切换，其他窗口可以显示在其上方。会关闭全屏。',
    ],
    'ja': [
        'ボーダーレス全画面',
        'ゲームを枠のないウィンドウでモニター全体に表示します：Alt+Tab がすぐに切り替わり、ほかのウィンドウを上に重ねられます。'
        '全画面はオフになります。',
    ],
    'ko': [
        '테두리 없는 전체 화면',
        '게임을 모니터 전체를 덮는 테두리 없는 창으로 표시합니다: Alt+Tab이 즉시 전환되고 다른 창을 위에 띄울 수 있습니다. '
        '전체 화면을 끕니다.',
    ],
}

for _language, _texts in TEXTS.items():
    assert len(_texts) == len(KEYS), (_language, len(_texts), len(KEYS))
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(zip(KEYS, _texts))
