# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of exclusive fullscreen (Display > Screen)."""

KEYS = (
    'Exclusive fullscreen',
    'Fullscreen takes the display over instead of covering the desktop: frames can arrive more evenly, '
    'but switching to another window takes a moment. Only with Fullscreen on.',
)
TEXTS = {
    'en': KEYS,
    'ru': (
        'Эксклюзивный полноэкранный режим',
        'Полный экран захватывает дисплей, а не просто закрывает рабочий стол: кадры могут приходить '
        'равномернее, но переключение на другое окно занимает мгновение. Только при включённом полном экране.',
    ),
    'ar': (
        'ملء الشاشة الحصري',
        'يستحوذ ملء الشاشة على الشاشة بدل تغطية سطح المكتب: قد تصل الإطارات بانتظام أكبر، لكن التبديل إلى '
        'نافذة أخرى يستغرق لحظة. فقط عند تشغيل ملء الشاشة.',
    ),
    'es': (
        'Pantalla completa exclusiva',
        'La pantalla completa toma el control de la pantalla en lugar de cubrir el escritorio: los fotogramas '
        'pueden llegar de forma más regular, pero cambiar a otra ventana tarda un momento. Solo con Pantalla '
        'completa activada.',
    ),
    'pt': (
        'Tela cheia exclusiva',
        'A tela cheia assume a tela em vez de cobrir a área de trabalho: os quadros podem chegar de forma mais '
        'regular, mas trocar para outra janela leva um instante. Só com Tela cheia ligada.',
    ),
    'fr': (
        'Plein écran exclusif',
        'Le plein écran prend le contrôle de l\'écran au lieu de couvrir le bureau : les images peuvent arriver '
        'plus régulièrement, mais passer à une autre fenêtre prend un instant. Seulement avec Plein écran activé.',
    ),
    'de': (
        'Exklusiver Vollbildmodus',
        'Der Vollbildmodus übernimmt den Bildschirm, statt den Desktop zu überdecken: Bilder kommen '
        'gleichmäßiger an, aber der Wechsel zu einem anderen Fenster dauert einen Moment. Nur mit Vollbild.',
    ),
    'it': (
        'Schermo intero esclusivo',
        'Lo schermo intero prende il controllo dello schermo invece di coprire il desktop: i fotogrammi possono '
        'arrivare in modo più regolare, ma passare a un\'altra finestra richiede un attimo. Solo con Schermo '
        'intero attivo.',
    ),
    'pl': (
        'Wyłączny pełny ekran',
        'Pełny ekran przejmuje wyświetlacz zamiast zakrywać pulpit: klatki mogą docierać równiej, ale '
        'przełączenie na inne okno trwa chwilę. Tylko z włączonym pełnym ekranem.',
    ),
    'tr': (
        'Özel tam ekran',
        'Tam ekran masaüstünü örtmek yerine ekranı devralır: kareler daha düzenli gelebilir, ancak başka bir '
        'pencereye geçmek biraz sürer. Yalnızca Tam ekran açıkken.',
    ),
    'zh': (
        '独占全屏',
        '全屏会接管显示器，而不是覆盖桌面：帧的到达可能更均匀，但切换到其他窗口需要片刻。仅在开启全屏时有效。',
    ),
    'ja': (
        '排他的フルスクリーン',
        'フルスクリーンがデスクトップを覆う代わりにディスプレイを占有します。フレームがより均等に届くことがありますが、'
        '他のウィンドウへの切り替えに少し時間がかかります。フルスクリーンがオンのときのみ。',
    ),
    'ko': (
        '전용 전체 화면',
        '전체 화면이 바탕 화면을 덮는 대신 디스플레이를 독점합니다. 프레임이 더 고르게 도착할 수 있지만 다른 창으로 '
        '전환하는 데 잠시 걸립니다. 전체 화면이 켜져 있을 때만 적용됩니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
