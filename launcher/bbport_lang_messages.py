# SPDX-License-Identifier: GPL-2.0-or-later
"""Localized texts of the launcher's messages about the game files (the Help topic for a program file that is
not the original)."""

KEYS = (
    'A program file of the game is not the original',
    'eboot.bin or a file in sce_module was converted by another tool or comes from another release (the log '
    'says "is not the original file"). The port reads them as they come from a dump of your own game.',
    'Choose that folder in the launcher: eboot.bin is exactly 93,825,704 bytes, and sce_module comes from the '
    'same dump.',
)
TEXTS = {
    'en': KEYS,
    'de': (
        'Eine Programmdatei des Spiels ist nicht das Original',
        'eboot.bin oder eine Datei in sce_module wurde von einem anderen Tool umgewandelt oder stammt aus einem '
        'anderen Release (im Log steht „is not the original file“). Der Port liest sie so, wie sie aus einem Dump '
        'deines eigenen Spiels kommen.',
        'Wähle diesen Ordner im Launcher: eboot.bin hat genau 93.825.704 Bytes, und sce_module stammt aus demselben '
        'Dump.',
    ),
    'ru': (
        'Программный файл игры не оригинальный',
        'eboot.bin или файл в sce_module преобразован другой программой или взят из другого релиза (в журнале: '
        '«is not the original file»). Порт читает их такими, какими они получены из дампа вашей собственной игры.',
        'Выберите эту папку в лаунчере: размер eboot.bin ровно 93 825 704 байта, а sce_module из того же дампа.',
    ),
    'ar': (
        'أحد ملفات برنامج اللعبة ليس الأصلي',
        'تم تحويل eboot.bin أو ملف في sce_module بأداة أخرى أو أنه من إصدار آخر (يذكر السجل «is not the original '
        'file»). يقرأ المنفذ هذه الملفات كما تأتي من تفريغ نسختك الخاصة من اللعبة.',
        'اختر هذا المجلد في المشغّل: حجم eboot.bin هو 93,825,704 بايت تمامًا، ومجلد sce_module من التفريغ نفسه.',
    ),
    'es': (
        'Un archivo de programa del juego no es el original',
        'eboot.bin o un archivo de sce_module fue convertido por otra herramienta o viene de otro lanzamiento (el '
        'registro dice «is not the original file»). El port los lee tal como salen de un volcado de tu propio juego.',
        'Elige esa carpeta en el launcher: eboot.bin mide exactamente 93.825.704 bytes y sce_module viene del mismo '
        'volcado.',
    ),
    'pt': (
        'Um arquivo de programa do jogo não é o original',
        'O eboot.bin ou um arquivo em sce_module foi convertido por outra ferramenta ou vem de outro lançamento (o '
        'log diz "is not the original file"). O port os lê como saem de um dump do seu próprio jogo.',
        'Escolha essa pasta no launcher: o eboot.bin tem exatamente 93.825.704 bytes, e a sce_module vem do mesmo '
        'dump.',
    ),
    'fr': (
        "Un fichier programme du jeu n'est pas l'original",
        "eboot.bin ou un fichier de sce_module a été converti par un autre outil ou provient d'une autre version "
        "(le journal indique « is not the original file »). Le port les lit tels qu'ils sortent d'un dump de votre "
        'propre jeu.',
        'Choisissez ce dossier dans le launcher : eboot.bin fait exactement 93 825 704 octets, et sce_module vient '
        'du même dump.',
    ),
    'it': (
        "Un file di programma del gioco non è l'originale",
        'eboot.bin o un file in sce_module è stato convertito da un altro strumento o viene da un’altra release (il '
        'log dice "is not the original file"). Il port li legge come escono da un dump del tuo gioco.',
        'Scegli quella cartella nel launcher: eboot.bin è esattamente 93.825.704 byte e sce_module viene dallo '
        'stesso dump.',
    ),
    'pl': (
        'Plik programu gry nie jest oryginalny',
        'eboot.bin lub plik w sce_module został przekonwertowany innym narzędziem albo pochodzi z innego wydania '
        '(w logu: „is not the original file”). Port odczytuje je tak, jak wychodzą ze zrzutu twojej własnej gry.',
        'Wybierz ten folder w launcherze: eboot.bin ma dokładnie 93 825 704 bajty, a sce_module pochodzi z tego '
        'samego zrzutu.',
    ),
    'tr': (
        'Oyunun bir program dosyası orijinal değil',
        'eboot.bin veya sce_module içindeki bir dosya başka bir araçla dönüştürülmüş ya da başka bir sürümden '
        'gelmiş (günlükte "is not the original file" yazar). Port bunları kendi oyununuzun dökümünden çıktığı '
        'gibi okur.',
        'Başlatıcıda bu klasörü seçin: eboot.bin tam olarak 93.825.704 bayttır ve sce_module aynı dökümden gelir.',
    ),
    'zh': (
        '游戏的某个程序文件不是原始文件',
        'eboot.bin 或 sce_module 中的某个文件被其他工具转换过，或来自其他版本（日志中显示“is not the original '
        'file”）。移植版按你自己游戏的转储原样读取这些文件。',
        '在启动器中选择该文件夹：eboot.bin 正好是 93,825,704 字节，sce_module 来自同一份转储。',
    ),
    'ja': (
        'ゲームのプログラムファイルがオリジナルではありません',
        'eboot.bin または sce_module 内のファイルが別のツールで変換されたか、別のリリースのものです（ログに「is '
        'not the original file」と表示されます）。移植版は、ご自身のゲームのダンプから取り出したままの状態で読み込みます。',
        'ランチャーでそのフォルダーを選んでください：eboot.bin はちょうど 93,825,704 バイトで、sce_module は同じ'
        'ダンプのものです。',
    ),
    'ko': (
        '게임의 프로그램 파일이 원본이 아닙니다',
        'eboot.bin 또는 sce_module의 파일이 다른 도구로 변환되었거나 다른 릴리스에서 왔습니다(로그에 "is not the '
        'original file"이라고 나옵니다). 포트는 이 파일들을 직접 만든 게임 덤프 그대로 읽습니다.',
        '런처에서 그 폴더를 선택하세요: eboot.bin은 정확히 93,825,704바이트이고 sce_module은 같은 덤프에서 '
        '나온 것입니다.',
    ),
}
TRANSLATIONS = {language: dict(zip(KEYS, texts, strict=True)) for language, texts in TEXTS.items()}
