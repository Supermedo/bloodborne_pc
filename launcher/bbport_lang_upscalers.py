# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized notes in the upscaler and frame generation lists: an option this graphics card cannot run
(hardware.unavailable) and the upscaler the automatic settings pick (bbport_fsr41_ui.upscaler_choice)."""

KEYS = (
    'not on this graphics card',
    'recommended for this PC',
)
TEXTS = {
    'en': KEYS,
    'ru': ('не для этой видеокарты', 'рекомендуется для этого ПК'),
    'ar': ('غير متاح على بطاقة الرسومات هذه', 'موصى به لهذا الحاسوب'),
    'es': ('no en esta tarjeta gráfica', 'recomendado para este PC'),
    'pt': ('não nesta placa de vídeo', 'recomendado para este PC'),
    'fr': ('pas sur cette carte graphique', 'recommandé pour ce PC'),
    'de': ('nicht auf dieser Grafikkarte', 'empfohlen für diesen PC'),
    'it': ('non su questa scheda video', 'consigliato per questo PC'),
    'pl': ('nie na tej karcie graficznej', 'zalecane dla tego PC'),
    'tr': ('bu ekran kartında yok', 'bu bilgisayar için önerilen'),
    'zh': ('此显卡不支持', '推荐用于此电脑'),
    'ja': ('このグラフィックカードでは使えません', 'このPCにおすすめ'),
    'ko': ('이 그래픽 카드에서는 사용 불가', '이 PC에 권장'),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
