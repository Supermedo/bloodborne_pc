# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized status line after an update adjusted the automatic graphics settings
(bbport_setup.update_auto_settings)."""

KEYS = (
    'Update: the graphics settings the launcher had made were adjusted to this version.',
)
TEXTS = {
    'en': KEYS,
    'ru': ('Обновление: настройки графики, подобранные лаунчером, приведены в соответствие с этой версией.',),
    'ar': ('تحديث: عُدّلت إعدادات الرسومات التي اختارها المشغّل لتناسب هذا الإصدار.',),
    'es': ('Actualización: los ajustes gráficos que había elegido el launcher se adaptaron a esta versión.',),
    'pt': ('Atualização: as configurações gráficas definidas pelo launcher foram ajustadas a esta versão.',),
    'fr': ('Mise à jour : les réglages graphiques choisis par le launcher ont été adaptés à cette version.',),
    'de': ('Update: Die Grafikeinstellungen, die der Launcher gewählt hatte, wurden an diese Version angepasst.',),
    'it': ('Aggiornamento: le impostazioni grafiche scelte dal launcher sono state adattate a questa versione.',),
    'pl': ('Aktualizacja: ustawienia grafiki wybrane przez launcher zostały dopasowane do tej wersji.',),
    'tr': ('Güncelleme: başlatıcının seçtiği grafik ayarları bu sürüme göre uyarlandı.',),
    'zh': ('更新：启动器之前设定的图形设置已按此版本调整。',),
    'ja': ('アップデート：ランチャーが設定したグラフィック設定をこのバージョンに合わせて調整しました。',),
    'ko': ('업데이트: 런처가 정했던 그래픽 설정을 이 버전에 맞게 조정했습니다.',),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
