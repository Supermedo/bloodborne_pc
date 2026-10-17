# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized opt-in motion-vector cache control; settings store a boolean."""

KEYS = (
    'Motion-vector cache (experimental)',
    'May improve FSR performance; uses extra VRAM. Requires object motion vectors. '
    'Applies at next game start.',
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Bewegungsvektor-Cache (experimentell)',
        'Kann die FSR-Leistung verbessern; benötigt zusätzlichen Grafikspeicher. '
        'Erfordert Objekt-Bewegungsvektoren. Gilt ab dem nächsten Spielstart.',
    ),
    'ru': (
        'Кэш векторов движения (экспериментально)',
        'Может повысить производительность FSR; требует дополнительной видеопамяти. '
        'Нужны векторы движения объектов. Применяется при следующем запуске игры.',
    ),
    'ar': (
        'ذاكرة متجهات الحركة المؤقتة (تجريبي)',
        'قد يحسّن أداء FSR؛ يستخدم ذاكرة رسومية إضافية. يتطلب متجهات حركة الأجسام. '
        'يُطبّق عند تشغيل اللعبة التالي.',
    ),
    'es': (
        'Caché de vectores de movimiento (experimental)',
        'Puede mejorar el rendimiento de FSR; usa VRAM adicional. Requiere vectores '
        'de movimiento de objetos. Se aplica al volver a iniciar el juego.',
    ),
    'pt': (
        'Cache de vetores de movimento (experimental)',
        'Pode melhorar o desempenho do FSR; usa VRAM adicional. Requer vetores de '
        'movimento de objetos. Aplicado na próxima inicialização do jogo.',
    ),
    'fr': (
        'Cache des vecteurs de mouvement (expérimental)',
        'Peut améliorer les performances de FSR ; utilise davantage de VRAM. Nécessite '
        'les vecteurs de mouvement des objets. Appliqué au prochain lancement du jeu.',
    ),
    'it': (
        'Cache dei vettori di movimento (sperimentale)',
        'Può migliorare le prestazioni di FSR; usa VRAM aggiuntiva. Richiede i vettori '
        'di movimento degli oggetti. Si applica al prossimo avvio del gioco.',
    ),
    'pl': (
        'Pamięć podręczna wektorów ruchu (eksperymentalna)',
        'Może zwiększyć wydajność FSR; zużywa dodatkową pamięć VRAM. Wymaga wektorów '
        'ruchu obiektów. Obowiązuje od następnego uruchomienia gry.',
    ),
    'tr': (
        'Hareket vektörü önbelleği (deneysel)',
        'FSR performansını artırabilir; ek VRAM kullanır. Nesne hareket vektörleri '
        'gerektirir. Oyunun bir sonraki açılışında uygulanır.',
    ),
    'zh': (
        '运动矢量缓存（实验性）',
        '可能提升 FSR 性能；需要额外显存。需要启用物体运动矢量。下次启动游戏时生效。',
    ),
    'ja': (
        'モーションベクトルキャッシュ（実験的）',
        'FSR の性能が向上する場合があります。追加の VRAM を使用します。オブジェクトのモーション'
        'ベクトルが必要です。次回ゲーム起動時に適用されます。',
    ),
    'ko': (
        '모션 벡터 캐시 (실험적)',
        'FSR 성능이 향상될 수 있으며 추가 VRAM을 사용합니다. 오브젝트 모션 벡터가 필요합니다. '
        '다음 게임 시작 시 적용됩니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
