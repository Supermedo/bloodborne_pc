# SPDX-License-Identifier: GPL-2.0-or-later
"""Exit-backup controls and category, separate from the game's language."""
import bbport_lang

TEXTS = {
    'zh': {
        'Back up after every game exit': '每次退出游戏后备份',
        'Copies the final save after a normal exit or crash. No periodic copies; exit backups are kept.':
            '正常退出或崩溃后复制最终存档，不在游玩中定时备份；退出备份会一直保留。',
        'After playing': '退出时备份',
    },
    'ru': {
        'Back up after every game exit': 'Создавать копию после каждого выхода из игры',
        'Copies the final save after a normal exit or crash. No periodic copies; exit backups are kept.':
            'Копирует итоговое сохранение после выхода или сбоя. Без копий во время игры; копии после выхода сохраняются.',
        'After playing': 'После игры',
    },
    'pt': {
        'Back up after every game exit': 'Fazer backup após cada saída do jogo',
        'Copies the final save after a normal exit or crash. No periodic copies; exit backups are kept.':
            'Copia o save final após sair ou após uma falha. Sem cópias periódicas; os backups de saída são mantidos.',
        'After playing': 'Após jogar',
    },
    'es': {
        'Back up after every game exit': 'Crear una copia tras cada salida del juego',
        'Copies the final save after a normal exit or crash. No periodic copies; exit backups are kept.':
            'Copia el guardado final al salir o tras un fallo. Sin copias periódicas; se conservan las copias de salida.',
        'After playing': 'Después de jugar',
    },
}
for language, texts in TEXTS.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(language, {}).update(texts)
    bbport_lang._tables.pop(language, None)
