# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the Sharp UI setting (All graphics). Russian is beside the English in
bbport_launcher_win.py."""

KEYS = (
    'Sharp UI',
    'Menus, HUD and text drawn at the output resolution instead of 1080p stretched up: much '
    'sharper at 1440p and 4K. From the next start, with live resolution changes off; not on '
    "ultrawide screens yet. An idea from Awaken's Lantern Engine.",
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Scharfe Oberfläche',
        'Menüs, HUD und Text werden in der Ausgabeauflösung gezeichnet statt aus 1080p hochgezogen: viel '
        'schärfer in 1440p und 4K. Ab dem nächsten Start, bei ausgeschalteter Auflösungsänderung im Spiel; '
        'auf Ultrawide-Bildschirmen noch nicht. Eine Idee aus Awakens Lantern Engine.',
    ),
    'ar': (
        'واجهة حادة',
        'تُرسم القوائم وواجهة اللعب والنصوص بدقة الإخراج بدلًا من تمديد 1080p: أوضح بكثير في 1440p و4K. '
        'من التشغيل التالي، مع إيقاف تغيير الدقة أثناء اللعب؛ ليس على الشاشات فائقة العرض بعد. فكرة من '
        'Lantern Engine الخاص بـ Awaken.',
    ),
    'es': (
        'Interfaz nítida',
        'Menús, HUD y texto se dibujan a la resolución de salida en lugar de estirarse desde 1080p: mucho '
        'más nítidos en 1440p y 4K. Desde el próximo inicio, con los cambios de resolución en vivo '
        'desactivados; aún no en pantallas ultrapanorámicas. Una idea del Lantern Engine de Awaken.',
    ),
    'pt': (
        'Interface nítida',
        'Menus, HUD e texto desenhados na resolução de saída em vez de esticados a partir de 1080p: muito '
        'mais nítidos em 1440p e 4K. A partir do próximo início, com as mudanças de resolução ao vivo '
        'desligadas; ainda não em ecrãs ultrawide. Uma ideia do Lantern Engine de Awaken.',
    ),
    'fr': (
        'Interface nette',
        "Menus, HUD et textes dessinés à la résolution de sortie au lieu d'être étirés depuis 1080p : bien "
        'plus nets en 1440p et 4K. Dès le prochain lancement, avec les changements de résolution en direct '
        "désactivés ; pas encore sur les écrans ultra-larges. Une idée du Lantern Engine d'Awaken.",
    ),
    'it': (
        'Interfaccia nitida',
        "Menu, HUD e testo disegnati alla risoluzione di uscita invece che allungati da 1080p: molto più "
        "nitidi a 1440p e 4K. Dal prossimo avvio, con i cambi di risoluzione dal vivo disattivati; non "
        "ancora sugli schermi ultrawide. Un'idea dal Lantern Engine di Awaken.",
    ),
    'pl': (
        'Ostry interfejs',
        'Menu, HUD i tekst rysowane w rozdzielczości wyjściowej zamiast rozciągania z 1080p: znacznie '
        'ostrzejsze w 1440p i 4K. Od następnego uruchomienia, przy wyłączonej zmianie rozdzielczości na '
        'żywo; jeszcze nie na ekranach ultrapanoramicznych. Pomysł z Lantern Engine od Awakena.',
    ),
    'tr': (
        'Keskin arayüz',
        'Menüler, HUD ve metin 1080p\'den büyütülmek yerine çıkış çözünürlüğünde çizilir: 1440p ve 4K\'da '
        'çok daha keskin. Bir sonraki başlatmadan itibaren, canlı çözünürlük değişiklikleri kapalıyken; '
        'ultra geniş ekranlarda henüz değil. Awaken\'ın Lantern Engine\'inden bir fikir.',
    ),
    'zh': (
        '清晰界面',
        '菜单、HUD 和文字以输出分辨率绘制，而不是从 1080p 拉伸：在 1440p 和 4K 下清晰得多。下次启动时'
        '生效，需关闭实时分辨率更改；暂不支持超宽屏。创意来自 Awaken 的 Lantern Engine。',
    ),
    'ja': (
        'シャープ UI',
        'メニュー、HUD、文字を 1080p から引き伸ばさず出力解像度で描画します：1440p と 4K でずっと鮮明に'
        'なります。次回の起動から、ライブ解像度変更をオフにした状態で有効；ウルトラワイド画面にはまだ'
        '対応していません。Awaken の Lantern Engine のアイデアです。',
    ),
    'ko': (
        '선명한 UI',
        '메뉴, HUD, 텍스트를 1080p에서 늘리지 않고 출력 해상도로 그립니다: 1440p와 4K에서 훨씬 선명합니다. '
        '다음 실행부터, 실시간 해상도 변경을 끈 상태에서 적용; 울트라와이드 화면은 아직 지원하지 않습니다. '
        'Awaken의 Lantern Engine에서 온 아이디어입니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
