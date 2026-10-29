# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized opt-in motion-vector cache control; settings store a boolean."""

KEYS = (
    'Motion-vector cache (experimental)',
    'May solve general FSR FPS drops; uses extra VRAM. Requires object motion vectors. '
    'Applies at next game start.',
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Bewegungsvektor-Cache (experimentell)',
        'Kann allgemeine FSR-FPS-Einbrüche beheben; benötigt zusätzlichen Grafikspeicher. '
        'Erfordert Objekt-Bewegungsvektoren. Gilt ab dem nächsten Spielstart.',
    ),
    'ru': (
        'Кэш векторов движения (экспериментально)',
        'Может устранить общие просадки FPS с FSR; требует дополнительной видеопамяти. '
        'Нужны векторы движения объектов. Применяется при следующем запуске игры.',
    ),
    'ar': (
        'ذاكرة متجهات الحركة المؤقتة (تجريبي)',
        'قد يعالج انخفاض معدل الإطارات العام مع FSR؛ يستخدم ذاكرة رسومية إضافية. يتطلب متجهات حركة الأجسام. '
        'يُطبّق عند تشغيل اللعبة التالي.',
    ),
    'es': (
        'Caché de vectores de movimiento (experimental)',
        'Puede solucionar caídas generales de FPS con FSR; usa VRAM adicional. Requiere vectores '
        'de movimiento de objetos. Se aplica al volver a iniciar el juego.',
    ),
    'pt': (
        'Cache de vetores de movimento (experimental)',
        'Pode resolver quedas gerais de FPS com FSR; usa VRAM adicional. Requer vetores de '
        'movimento de objetos. Aplicado na próxima inicialização do jogo.',
    ),
    'fr': (
        'Cache des vecteurs de mouvement (expérimental)',
        'Peut résoudre les baisses générales de FPS avec FSR ; utilise davantage de VRAM. Nécessite '
        'les vecteurs de mouvement des objets. Appliqué au prochain lancement du jeu.',
    ),
    'it': (
        'Cache dei vettori di movimento (sperimentale)',
        'Può risolvere i cali generali di FPS con FSR; usa VRAM aggiuntiva. Richiede i vettori '
        'di movimento degli oggetti. Si applica al prossimo avvio del gioco.',
    ),
    'pl': (
        'Pamięć podręczna wektorów ruchu (eksperymentalna)',
        'Może rozwiązać ogólne spadki FPS z FSR; zużywa dodatkową pamięć VRAM. Wymaga wektorów '
        'ruchu obiektów. Obowiązuje od następnego uruchomienia gry.',
    ),
    'tr': (
        'Hareket vektörü önbelleği (deneysel)',
        'FSR ile genel FPS düşüşlerini giderebilir; ek VRAM kullanır. Nesne hareket vektörleri '
        'gerektirir. Oyunun bir sonraki açılışında uygulanır.',
    ),
    'zh': (
        '运动矢量缓存（实验性）',
        '可能解决 FSR 的一般帧率下降问题；需要额外显存。需要启用物体运动矢量。下次启动游戏时生效。',
    ),
    'ja': (
        'モーションベクトルキャッシュ（実験的）',
        'FSR 使用時の一般的な FPS 低下を解消する場合があります。追加の VRAM を使用します。オブジェクトのモーション'
        'ベクトルが必要です。次回ゲーム起動時に適用されます。',
    ),
    'ko': (
        '모션 벡터 캐시 (실험적)',
        'FSR 사용 시 일반적인 FPS 저하를 해결할 수 있으며 추가 VRAM을 사용합니다. 오브젝트 모션 벡터가 필요합니다. '
        '다음 게임 시작 시 적용됩니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
