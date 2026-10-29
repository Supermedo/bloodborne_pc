# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the backups made after the game exits (Saves > Backups)."""

KEYS = (
    'Back up after every game exit',
    'Copies the final save after a normal exit or crash. No periodic copies; exit backups are kept.',
    'After playing',
)
TEXTS = {
    'en': KEYS,
    'ru': (
        'Создавать копию после каждого выхода из игры',
        'Копирует итоговое сохранение после выхода или сбоя. Без копий во время игры; копии после выхода '
        'сохраняются.',
        'После игры',
    ),
    'ar': (
        'نسخة احتياطية بعد كل خروج من اللعبة',
        'ينسخ الحفظ الأخير بعد الخروج العادي أو الانهيار. لا نسخ دورية أثناء اللعب؛ نسخ الخروج تُحفظ دائمًا.',
        'بعد اللعب',
    ),
    'es': (
        'Crear una copia tras cada salida del juego',
        'Copia el guardado final al salir o tras un fallo. Sin copias periódicas; se conservan las copias de '
        'salida.',
        'Después de jugar',
    ),
    'pt': (
        'Fazer backup após cada saída do jogo',
        'Copia o save final após sair ou após uma falha. Sem cópias periódicas; os backups de saída são mantidos.',
        'Após jogar',
    ),
    'fr': (
        'Sauvegarder après chaque sortie du jeu',
        'Copie la sauvegarde finale après une sortie normale ou un plantage. Pas de copies périodiques ; les '
        'sauvegardes de sortie sont conservées.',
        'Après la partie',
    ),
    'de': (
        'Nach jedem Beenden des Spiels sichern',
        'Kopiert den letzten Spielstand nach normalem Beenden oder einem Absturz. Keine Kopien während des '
        'Spielens; Sicherungen nach dem Spielen bleiben erhalten.',
        'Nach dem Spielen',
    ),
    'it': (
        'Backup dopo ogni uscita dal gioco',
        'Copia il salvataggio finale dopo un\'uscita normale o un crash. Nessuna copia periodica; i backup di '
        'uscita vengono conservati.',
        'Dopo aver giocato',
    ),
    'pl': (
        'Kopia zapasowa po każdym wyjściu z gry',
        'Kopiuje ostatni zapis po zwykłym wyjściu lub awarii. Bez okresowych kopii; kopie po wyjściu są '
        'zachowywane.',
        'Po grze',
    ),
    'tr': (
        'Oyundan her çıkışta yedekle',
        'Normal çıkıştan veya çökmeden sonra son kaydı kopyalar. Oynarken düzenli kopya alınmaz; çıkış '
        'yedekleri saklanır.',
        'Oynadıktan sonra',
    ),
    'zh': (
        '每次退出游戏后备份',
        '正常退出或崩溃后复制最终存档，不在游玩中定时备份；退出备份会一直保留。',
        '退出时备份',
    ),
    'ja': (
        'ゲーム終了のたびにバックアップ',
        '通常終了またはクラッシュの後に最終セーブをコピーします。プレイ中の定期コピーはありません。終了時のバックアップは保持されます。',
        'プレイ後',
    ),
    'ko': (
        '게임을 종료할 때마다 백업',
        '정상 종료 또는 충돌 후 마지막 저장을 복사합니다. 플레이 중 주기적인 복사는 없으며, 종료 백업은 보관됩니다.',
        '플레이 후',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
