# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher translations. The Russian text is the key; English is looked up by it.

ui_language in the launcher settings: "ru", "en" or "" (the system locale: Russian for ru_*,
English otherwise).
"""
import os

EN = {
    # Window, pages, buttons
    "Запустить": "Play",
    "Остановить": "Stop",
    "Настройки": "Settings",
    "Журнал": "Log",
    "Не удалось запустить: {}": "Could not start: {}",
    "\n— игра завершилась (код {}) —\n": "\n— the game exited (code {}) —\n",
    # Launcher language
    "Язык лаунчера": "Launcher language",
    "Как в системе": "System",
    "Применится после перезапуска лаунчера, пока игра запущена":
        "Applies after restarting the launcher while the game is running",
    # Game
    "Игра": "Game",
    "Папка игры (CUSA03173)": "Game folder (CUSA03173)",
    "Выбрать папку с eboot.bin": "Choose the folder with eboot.bin",
    "Открыть в файловом менеджере": "Open in the file manager",
    "Папка сохранений": "Saves folder",
    "Выбрать папку сохранений": "Choose the saves folder",
    "Вернуть папку по умолчанию": "Back to the default folder",
    "Язык системы": "System language",
    "По умолчанию: {}": "Default: {}",
    "Найдены сохранения": "Saves found",
    "Сохранений пока нет: игра создаст их здесь": "No saves yet: the game will create them here",
    "не выбрана": "not chosen",
    "Найден eboot.bin": "eboot.bin found",
    "Нет eboot.bin в папке": "No eboot.bin in the folder",
    "Папка игры (с eboot.bin)": "Game folder (with eboot.bin)",
    # Languages
    "Английский": "English",
    "Русский": "Russian",
    "Японский": "Japanese",
    "Французский": "French",
    "Испанский": "Spanish",
    "Немецкий": "German",
    "Итальянский": "Italian",
    # Mods
    "Моды": "Mods",
    "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
    "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске.":
        "Extract each mod into its own folder (with dvdroot_ps4, or chr/, parts/, ... directly). "
        "When files collide, the mod lower in the list wins. Applied at start.",
    "Загружать моды": "Load mods",
    "Папка модов": "Mods folder",
    "Выбрать папку модов": "Choose the mods folder",
    "Открыть папку модов": "Open the mods folder",
    "Обновить список": "Refresh the list",
    "Загрузить раньше": "Load earlier",
    "Загрузить позже": "Load later",
    "Модов нет": "No mods",
    # Patches
    "Сторонние патчи": "Third-party patches",
    "XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. Применяются при запуске.":
        "shadPS4-format XML patches for version 01.09 from the patches folder. Applied at start.",
    "Папка патчей": "Patches folder",
    "Выбрать папку патчей": "Choose the patches folder",
    "Открыть папку патчей": "Open the patches folder",
    "Патчей нет": "No patches",
    "Автор: {}": "Author: {}",
    # Screen
    "Экран": "Display",
    "Разрешение вывода": "Output resolution",
    "Апскейлер дорисовывает кадр; Steam Deck — 720p": "The upscaler fills the frame; Steam Deck: 720p",
    "Полноэкранный режим": "Fullscreen",
    "Смена разрешения на лету": "Live resolution changes",
    "Без перезапуска, но медленнее на Steam Deck и старых GPU":
        "No restart needed, but slower on the Steam Deck and older GPUs",
    "Авто (по видеокарте)": "Auto (by GPU)",
    "Выключена (быстрее)": "Off (faster)",
    "Включена": "On",
    "Режим показа кадров": "Present mode",
    "Разрешить HDR": "Allow HDR",
    # Upscaler
    "Апскейлер": "Upscaler",
    "Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)":
        "Stored in bbport.ini; in game, change it in the menu (Insert or L3+R3)",
    "TAA (нативное сглаживание)": "TAA (native anti-aliasing)",
    "Выключен": "Off",
    "Пресет": "Preset",
    "Резкость (RCAS)": "Sharpening (RCAS)",
    "Сила резкости": "Sharpness",
    "Векторы движения объектов": "Object motion vectors",
    "Меньше гостинга на персонажах; стоит около 10% FPS": "Less ghosting on characters; costs about 10% FPS",
    "Показывать FPS": "Show FPS",
    "Ассеты найдены": "Assets found",
    "Нет ассетов: tools/fetch_fsr4_assets.sh": "No assets: tools/fetch_fsr4_assets.sh",
    "Ассеты для выбранного режима найдены": "Assets for the selected mode found",
    "{}. Установите полный набор fsr4_411 в {}.": "{}. Install the full fsr4_411 set into {}.",
    "Сглаживание в разрешении вывода без модели FSR": "Anti-aliasing at the output resolution, no FSR model",
    "Нет или повреждён файл {}": "Missing or damaged file {}",
    # Effects
    "Эффекты игры": "Game effects",
    "Патчи игры, применяются при запуске": "Game patches, applied at start",
    "Детализация моделей": "Model detail",
    "Как в игре": "As in the game",
    "Максимальная (-2)": "Highest (-2)",
    "Ниже (1)": "Lower (1)",
    "Минимальная (2)": "Lowest (2)",
    "Хроматическая аберрация": "Chromatic aberration",
    "Глубина резкости (DoF)": "Depth of field (DoF)",
    "Размытие в движении": "Motion blur",
    "Затенение SSAO": "SSAO",
    "Собственное сглаживание игры": "The game's own anti-aliasing",
    "Тени от динамических источников": "Dynamic light shadows",
    "Отражения SSR (не было в игре)": "SSR reflections (not in the original game)",
    "Пропуск заставок при запуске": "Skip the intro videos",
    "Свободная камера (Cross + L3 / Space + Z)": "Free camera (Cross + L3 / Space + Z)",
    "Debug menu (левый touchpad / Tab; нужны шрифты)": "Debug menu (left touchpad / Tab; needs fonts)",
    "Установите DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода Nexus #253":
        "Install DbgFont14h.ccm and DbgFont14h.tpf into dvdroot_ps4/font from Nexus mod #253",
    # Frame rate
    "Частота кадров": "Frame rate",
    "Режим": "Mode",
    "Какой патч частоты кадров применить к игре": "Which frame rate patch to apply to the game",
    "Без ограничения (патч)": "Unlocked (patch)",
    "30 (как на PS4)": "30 (as on PS4)",
    "Ограничение FPS": "FPS limit",
    "0 — по частоте экрана (не выше 120 Гц); укажите число, чтобы ограничить иначе":
        "0: the display refresh rate (up to 120 Hz); set a number for another limit",
    # Performance
    "Производительность": "Performance",
    "Двухстадийный конвейер GPU": "Two-stage GPU pipeline",
    "Быстрее на 20–30%; при нестабильности выключите": "20–30% faster; turn off if unstable",
    "Авто (8+ потоков)": "Auto (8+ threads)",
    "Включён": "On",
    "Выключен (стабильнее)": "Off (more stable)",
    "Чтение данных GPU процессором": "GPU data readbacks by the CPU",
    "Relaxed (по умолчанию)": "Relaxed (default)",
    "Выключены": "Off",
    # Developer
    "Для разработчика": "Developer",
    "Статистика кадров в журнале": "Frame statistics in the log",
    "Профиль GPU в журнале": "GPU profile in the log",
    "Слои валидации Vulkan": "Vulkan validation layers",
    "Сильно замедляет": "Much slower",
    "Доп. переменные (ИМЯ=значение через пробел)": "Extra variables (NAME=value, space-separated)",
    # Online (bbport_linux_online)
    "Онлайн": "Online",
    "Призыв, послания и пятна крови других охотников. Применяется при запуске.":
        "Summons, messages and bloodstains of other hunters. Applied at start.",
    "Играть онлайн": "Play online",
    "Серверы supermedo.com с бесплатным аккаунтом (ниже)": "supermedo.com's own servers, with a free account (below)",
    "SuperMedo Seamless co-op (бета)": "SuperMedo Seamless co-op (beta)",
    "Только в Windows: BBSC — программа для Windows, Linux-версию игры она не запускает":
        "Windows only: BBSC is a Windows program and cannot start the Linux game",
    "Роутер сам открывает порт игры другим охотникам": "The router opens the game's port for other hunters by itself",
    "Соединение с сервером": "Connection to the server",
    "Проверить": "Test",
    "Проверка…": "Checking…",
    "Аккаунт SuperMedo": "SuperMedo account",
    "Аккаунт": "Account",
    "Войти через Discord": "Sign in with Discord",
    "Выйти": "Sign out",
    "Вход выполнен: {}": "Signed in as {}",
    "Вход не выполнен: войдите через Discord или создайте аккаунт ниже":
        "Not signed in: sign in with Discord, or create an account below",
    "Запуск входа…": "Starting the sign-in…",
    "Войдите через Discord на открывшейся странице и вернитесь сюда: {}":
        "Sign in with Discord in the page that opened, then come back here: {}",
    "Вход не удался: {}": "Sign-in failed: {}",
    # Sign-in with a password, fair play, the start steps from Windows
    "Пароль": "Password",
    "Вход по паролю": "Sign in with a password",
    "Войти": "Sign in",
    "Введите Online ID и пароль": "Enter your Online ID and password",
    "Нет связи с сервером: {}": "Cannot reach the server: {}",
    "Только офлайн: в онлайн-игре остаётся выключенным": "Offline only: stays off in online play",
    "Читы и функции отладки работают только офлайн — честно по отношению к другим игрокам. Сейчас включены: {}.":
        "Cheats and debug features run offline only, fair to the other players. On now: {}.",
    "Отмена": "Cancel",
    "Играть офлайн с ними": "Play offline with them",
    "Играть онлайн без них": "Play online without them",
    "Выход на рабочий стол в меню игры": "Quit to Desktop in the game's menus",
    "Настройки ПК в системном меню игры": "PC Settings in the game's System menu",
    "Перерождение у Куклы": "Rebirth at the Doll",
    "Поле зрения": "Field of view",
    "Градусы; в игре — 43": "Degrees; the game's is 43",
    "Положение HUD на ультрашироком экране": "HUD position on an ultrawide screen",
    "У краёв экрана": "Screen edges",
    "Посередине": "Halfway",
    "По центру (16:9)": "Centred (16:9)",
    "V-Sync и ограничение кадров чуть ниже частоты экрана": "V-Sync and a frame cap just below the refresh rate",
    "Низкая задержка": "Low latency",
    "Авто (NVIDIA: включена + Boost)": "Auto (NVIDIA: On + Boost)",
    "Выключена": "Off",
    "Включена + Boost": "On + Boost",
    "Сохранения": "Saves",
    "Копия сохранений при запуске": "Back up saves at start",
    "В папке save-backups рядом с сохранениями; хранятся последние 10":
        "In save-backups beside the saves; the newest 10 are kept",
    "Новый аккаунт SuperMedo": "New SuperMedo account",
    "Не нужен при входе через Discord: Discord сам создаёт аккаунт с вашим именем в Discord":
        "Not needed with Discord: signing in with Discord makes the account, named after your Discord name",
    "Новый Online ID (1–16 букв, цифр, _ или -)": "New Online ID (1 to 16 letters, numbers, _ or -)",
    "Новый пароль": "New password",
    "Эл. почта (только для создания аккаунта)": "Email address (only to make the account)",
    "Создать аккаунт": "Create account",
    "Создать": "Create",
    "Введите новый Online ID, пароль и адрес эл. почты": "Enter a new Online ID, a password and your email address",
    "Создание аккаунта…": "Creating the account…",
    "Сервер SuperMedo недоступен: {}": "SuperMedo Server could not be reached: {}",
    "Не удалось создать аккаунт: {}": "The account could not be created: {}",
    "Аккаунт {} создан, вход выполнен. Нажмите «Запустить»": "Account {} created and signed in. Press Play to go online",
    "Войдите в аккаунт SuperMedo на странице «Онлайн» или выключите онлайн":
        "Sign in to your SuperMedo account on the Online page, or turn online off",
    "Отряд (игра с друзьями)": "Party (play with friends)",
    "Пока вы в отряде, ваши знаки призыва и Колокол зова находят только охотников отряда, "
    "без ограничения по уровню. До 4 охотников.":
        "While you are in a party, your summon signs and your Beckoning Bell find only your party's hunters, "
        "with no level limit. Up to 4 hunters.",
    "Ваш отряд": "Your party",
    "Не в отряде": "Not in a party",
    "Обновить": "Refresh",
    "Код отряда": "Party code",
    "Отправьте код друзьям. Новый код отменяет старый; кто уже в отряде, остаётся":
        "Send this code to your friends. A new code stops the old one; friends already in stay",
    "Скопировать": "Copy",
    "Код скопирован": "Code copied",
    "Новый код": "New code",
    "Вступить по коду друга (SM-XXXX-XXXX)": "Join with a friend's code (SM-XXXX-XXXX)",
    "Создать код отряда": "Make party code",
    "Покинуть отряд": "Leave party",
    "Сначала войдите в аккаунт SuperMedo (выше)": "Sign in to your SuperMedo account first (above)",
    "Ответ сервера SuperMedo: {}": "SuperMedo Server said: {}",
    "В отряде {}/{}": "In the party {}/{}",
    "хост": "host",
    "в сети": "online",
    "не в сети": "offline",
    "уровень {}": "level {}",
    "Исключить": "Kick",
    "Это сохранения Seamless co-op из Windows (seamless-coop): без BBSC их предметы "
    "показываются как ?GoodsName?. Выберите обычную папку user":
        "These are Seamless co-op's saves from Windows (seamless-coop): without BBSC their items show as "
        "?GoodsName?. Choose the normal user folder",
}


def system_language():
    for key in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        value = os.environ.get(key, "")
        if value:
            return "ru" if value.lower().startswith("ru") else "en"
    return "en"


_language = "ru"


def set_language(choice):
    """choice: "ru", "en" or "" (the system's)."""
    global _language
    _language = choice if choice in ("ru", "en") else system_language()


def language():
    return _language


def tr(text):
    return EN.get(text, text) if _language == "en" else text
