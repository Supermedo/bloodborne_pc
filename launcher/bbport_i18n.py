# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher translations. The Russian text is the key; other languages are looked up by it.

ui_language in the launcher settings: "ru", "en", "pt_BR" or "" (the system locale:
Russian for ru_*, Brazilian Portuguese for pt_BR*, English otherwise).
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
    "Управление": "Controls",
    "Клавиатура": "Keyboard",
    "Геймпад": "Gamepad",
    "Назначение кнопок; применяется при запуске игры": "Button assignments; applied when the game starts",
    "Назначить": "Assign",
    "Сбросить": "Reset",
    "{} (по умолчанию)": "{} (default)",
    "не назначено": "not assigned",
    "Нажмите клавишу или кнопку… (Esc — отмена)": "Press a key or a button… (Esc cancels)",
    "Крест": "Cross",
    "Круг": "Circle",
    "Квадрат": "Square",
    "Треугольник": "Triangle",
    "Тачпад, левая половина (жесты)": "Touchpad, left half (gestures)",
    "Тачпад, правая половина (личные вещи)": "Touchpad, right half (key items)",
    "Крестовина вверх": "D-pad up",
    "Крестовина вниз": "D-pad down",
    "Крестовина влево": "D-pad left",
    "Крестовина вправо": "D-pad right",
    "Движение вперёд": "Move forward",
    "Движение назад": "Move back",
    "Движение влево": "Move left",
    "Движение вправо": "Move right",
    "Камера вверх": "Camera up",
    "Камера вниз": "Camera down",
    "Камера влево": "Camera left",
    "Камера вправо": "Camera right",
    "Контроллер": "Controller",
    "Выбранный берётся, как только подключится": "The chosen one is used as soon as it connects",
    "Первый подключённый": "First connected",
    "{} (не подключён)": "{} (not connected)",
    "Нужно обновление 1.09: скопируйте файлы дампа обновления 1.09 в папку игры с заменой (найдена версия {})":
        "The 1.09 update is needed: copy the dumped 1.09 update into the game folder, replacing files (found version {})",
    "eboot.bin не от версии 1.09: скопируйте eboot.bin из дампа обновления 1.09 в папку игры с заменой":
        "eboot.bin is not from 1.09: copy eboot.bin from the dumped 1.09 update into the game folder, replacing it",
    "Поддерживается только CUSA03173 с обновлением 1.09 (найдено {})":
        "Only CUSA03173 with update 1.09 is supported (found {})",
    "eboot.bin не читается как расшифрованный исполняемый файл PS4: сделайте дамп заново":
        "eboot.bin cannot be read as a decrypted PS4 executable: dump the game again",
    "Bloodborne CUSA03173, версия 1.09": "Bloodborne CUSA03173, version 1.09",
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
    "{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже":
        "{}. Build FSR 4.1.1 from your own DLL with the “Choose DLL…” button below",
    "FSR 4.1.1 из своей DLL AMD": "FSR 4.1.1 from your own AMD DLL",
    "Отменить": "Cancel",
    "Выбрать DLL…": "Choose DLL…",
    "DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)": "AMD upscaler DLL (amd_fidelityfx_upscaler_dx12.dll)",
    "нет": "none",
    "Эта DLL не подходит": "This DLL does not fit",
    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и подобные) устроены иначе и пока не поддерживаются.":
        "{}: version {}, FSR 4 models: {}.\n\nbbport replays AMD's official FSR 4.1.x only (model v07_fp8_no_scale): for example FSR4_LATEST from OptiScaler, or amd_fidelityfx_upscaler_dx12.dll from a game with FSR 4.1. Community builds (4.0.2b and the like) are laid out differently and are not supported yet.",
    "Загрузчик не подходит": "The loader does not fit",
    "Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или amd_fidelityfx_dx12.dll)":
        "FidelityFX loader (amd_fidelityfx_loader_dx12.dll or amd_fidelityfx_dx12.dll)",
    "\n— сборка FSR 4.1.1 из {} —\n": "\n— building FSR 4.1.1 from {} —\n",
    "Подготовка…": "Preparing…",
    "Запись проходов DLL под Proton: {} из {}": "Recording the DLL's passes under Proton: {} of {}",
    "Перевод проходов в SPIR-V…": "Translating the passes to SPIR-V…",
    "— сборка FSR 4.1.1 завершилась (код {}) —\n": "— FSR 4.1.1 build finished (code {}) —\n",
    "FSR 4.1.1 собран и выбран": "FSR 4.1.1 built and selected",
    "Сборка FSR 4.1.1 отменена": "FSR 4.1.1 build cancelled",
    "FSR 4.1.1 не собран": "FSR 4.1.1 was not built",
    "DLL не подходит: подробности в журнале": "The DLL does not fit: details in the log",
    "Не хватает программ для сборки: список в журнале": "Tools for the build are missing: the list is in the log",
    "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или новая видеокарта?): подробности в журнале":
        "The recorded passes do not match what bbport replays (another DLL version, or a new GPU?): details in the log",
    "Не найден загрузчик FidelityFX 2.x": "No FidelityFX loader 2.x found",
    "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с этим Proton, чтобы Steam поставил его рантайм":
        "No suitable Proton found: install GE-Proton 10 or newer (ProtonUp-Qt), or Proton Experimental / Proton-CachyOS in Steam. If you have them, start any game with that Proton in Steam so that Steam installs its runtime",
    "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте папку fsr4_411. Подробности в журнале":
        "No Proton started this DLL's FSR 4.1. Either they are too old (GE-Proton 10+, Proton Experimental, Proton-CachyOS 11 work), or the DLL does not enable FSR 4.1 on this GPU (Steam Deck?): then build on a PC with a Radeon RX 7000/9000 and copy the fsr4_411 folder. Details in the log",
    "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале":
        "On NixOS the recording runs on the system itself (systemd --user) with umu-launcher from nix-shell, and neither was found: install umu-launcher or Nix. Details in the log",
    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.":
        "{}: FSR {}. This is AMD's official FSR 4.0.x: it is enabled on RDNA4 GPUs (RX 9000) only and does not start under Proton on other GPUs, so it cannot be recorded.\n\nFSR 4.1.x is needed: for example FSR4_LATEST from OptiScaler, or the DLL of a game with FSR 4.1.",
    "{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll версии 4.1.x.":
        "{}: this DLL has no FSR 4 inside. amd_fidelityfx_upscaler_dx12.dll version 4.1.x is needed.",
    "Это не загрузчик":
        "This is not the loader",
    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll (у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и DLL апскейлера.":
        "{} is the upscaler DLL. The loader is called amd_fidelityfx_loader_dx12.dll (amd_fidelityfx_dx12.dll in OptiScaler) and lies in the same game folder as the upscaler DLL.",
    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL апскейлера.":
        "{}: version {}. A FidelityFX loader 2.x is needed, from the same game as the upscaler DLL.",
    "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры":
        "No loader next to the DLL: choose amd_fidelityfx_loader_dx12.dll from the game's folder",
    "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)":
        "Build FSR 4.1.1 from amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: the FSR4_LATEST folder, or from a game with FSR 4.1). Needs GE-Proton 10+, Proton Experimental or Proton-CachyOS; 2–5 minutes (RDNA4: twice as long, with the FP8 variant)",
    "Сборка не удалась (код {}): подробности в журнале": "The build failed (code {}): details in the log",
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
    "0 — без ограничения; укажите число, чтобы ограничить FPS":
        "0: no limit; set a number to cap the frame rate",
    # Performance
    "Производительность": "Performance",
    "Двухстадийный конвейер GPU": "Two-stage GPU pipeline",
    "Быстрее на 20–30%; при нестабильности выключите": "20–30% faster; turn off if unstable",
    "Авто": "Auto",
    "Включён при 8 и более потоках процессора": "On with 8 or more CPU threads",
    "Включён": "On",
    "Стабильнее, но медленнее": "More stable, but slower",
    "Чтение данных GPU процессором": "GPU data readbacks by the CPU",
    "По умолчанию": "Default",
    "Фоновая загрузка в видеопамять": "Background pre-upload into VRAM",
    "Меньше рывков при подгрузке зон": "Fewer hitches when areas stream in",
    "Обычная": "Normal",
    "Без лишней видеопамяти": "No extra VRAM",
    "Полная": "Full",
    "Около 3 ГБ видеопамяти сверху": "About 3 GB more VRAM",
    # Mode
    "Режим работы": "Mode",
    "Новая модель памяти и трансляции": "New memory and translation model",
    "Эксперимент, только видеокарты AMD. Видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются; возможны ошибки. Выключено — старая модель памяти, как в 0.3, со всеми исправлениями":
        "Experimental, AMD GPUs only. The GPU works with the game's memory directly, as in a PC game, and graphics commands are translated rather than emulated; errors are possible. Off: the old memory model, as in 0.3, with all the fixes",
    "Только для видеокарт AMD, а на этом компьютере её нет. Используется старая модель памяти, как в 0.3, со всеми исправлениями":
        "AMD GPUs only, and this computer has none. The old memory model is used, as in 0.3, with all the fixes",
    "Синхронизация как в 0.3": "Synchronisation as in 0.3",
    "Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3":
        "For finding regressions: the old memory model and copy waits as released in 0.3",
    "Выключена": "Off",
    "Выключены": "Off",
    # Developer
    "Для разработчика": "Developer",
    "Статистика кадров в журнале": "Frame statistics in the log",
    "Сохранять журнал и статистику в файл": "Save the log and statistics to a file",
    "Диагностика вылетов": "Crash diagnostics",
    "Проверяет кучу игры и записывает записи в её память; немного медленнее":
        "Checks the game's heap and logs writes into its memory; a little slower",
    "В папку logs в каталоге данных: для разбора рывков и вылетов":
        "Into the logs folder of the data directory: to look into stutters and crashes",
    "Профиль GPU в журнале": "GPU profile in the log",
    "Слои валидации Vulkan": "Vulkan validation layers",
    "Сильно замедляет": "Much slower",
    "Доп. переменные (ИМЯ=значение через пробел)": "Extra variables (NAME=value, space-separated)",
}

PT_BR = {
    # Window, pages, buttons
    "Запустить": "Jogar",
    "Остановить": "Parar",
    "Настройки": "Configurações",
    "Журнал": "Log",
    "Не удалось запустить: {}": "Não foi possível iniciar: {}",
    "\n— игра завершилась (код {}) —\n": "\n— o jogo foi encerrado (código {}) —\n",
    # Launcher language
    "Язык лаунчера": "Idioma do launcher",
    "Как в системе": "Sistema",
    "Применится после перезапуска лаунчера, пока игра запущена":
        "Será aplicado após reiniciar o launcher enquanto o jogo estiver em execução",
    # Game
    "Игра": "Jogo",
    "Папка игры (CUSA03173)": "Pasta do jogo (CUSA03173)",
    "Выбрать папку с eboot.bin": "Selecionar a pasta com eboot.bin",
    "Открыть в файловом менеджере": "Abrir no gerenciador de arquivos",
    "Папка сохранений": "Pasta de salvamentos",
    "Выбрать папку сохранений": "Selecionar a pasta de salvamentos",
    "Вернуть папку по умолчанию": "Restaurar a pasta padrão",
    "Язык системы": "Idioma do sistema",
    "По умолчанию: {}": "Padrão: {}",
    "Найдены сохранения": "Salvamentos encontrados",
    "Сохранений пока нет: игра создаст их здесь": "Ainda não há salvamentos: o jogo os criará aqui",
    "не выбрана": "não selecionada",
    "Найден eboot.bin": "eboot.bin encontrado",
    "Нет eboot.bin в папке": "Não há eboot.bin na pasta",
    "Папка игры (с eboot.bin)": "Pasta do jogo (com eboot.bin)",
    # Languages
    "Английский": "Inglês",
    "Русский": "Russo",
    "Японский": "Japonês",
    "Французский": "Francês",
    "Испанский": "Espanhol",
    "Немецкий": "Alemão",
    "Итальянский": "Italiano",
    "English": "Inglês",
    # Mods
    "Моды": "Mods",
    "Распакуйте каждый мод в отдельную папку (с dvdroot_ps4 или сразу с chr/, parts/ и т. п.). "
    "При совпадении файлов побеждает мод ниже в списке. Применяется при запуске.":
        "Extraia cada mod para sua própria pasta (com dvdroot_ps4 ou diretamente com chr/, parts/, ...). "
        "Em caso de conflito de arquivos, o mod que estiver mais abaixo na lista terá prioridade. "
        "Aplicado ao iniciar.",
    "Загружать моды": "Carregar mods",
    "Папка модов": "Pasta de mods",
    "Выбрать папку модов": "Selecionar a pasta de mods",
    "Открыть папку модов": "Abrir a pasta de mods",
    "Обновить список": "Atualizar a lista",
    "Загрузить раньше": "Carregar antes",
    "Загрузить позже": "Carregar depois",
    "Модов нет": "Nenhum mod",
    # Patches
    "Сторонние патчи": "Patches de terceiros",
    "XML-патчи в формате shadPS4 для версии 01.09 из папки патчей. Применяются при запуске.":
        "Patches XML no formato do shadPS4 para a versão 01.09, carregados da pasta de patches. "
        "Aplicados ao iniciar.",
    "Папка патчей": "Pasta de patches",
    "Выбрать папку патчей": "Selecionar a pasta de patches",
    "Открыть папку патчей": "Abrir a pasta de patches",
    "Патчей нет": "Nenhum patch",
    "Автор: {}": "Autor: {}",
    # Screen
    "Экран": "Tela",
    "Разрешение вывода": "Resolução de saída",
    "Апскейлер дорисовывает кадр; Steam Deck — 720p": "O upscaler preenche o quadro; Steam Deck: 720p",
    "Полноэкранный режим": "Tela cheia",
    "Смена разрешения на лету": "Alteração de resolução em tempo real",
    "Без перезапуска, но медленнее на Steam Deck и старых GPU":
        "Não exige reinicialização, mas é mais lento no Steam Deck e em GPUs mais antigas",
    "Авто (по видеокарте)": "Automático (pela GPU)",
    "Выключена (быстрее)": "Desativado (mais rápido)",
    "Включена": "Ativado",
    "Режим показа кадров": "Modo de apresentação",
    "Разрешить HDR": "Permitir HDR",
    # Upscaler
    "Апскейлер": "Upscaler",
    "Хранится в bbport.ini; в игре меняется через меню (Insert или L3+R3)":
        "Armazenado em bbport.ini; no jogo, altere pelo menu (Insert ou L3+R3)",
    "TAA (нативное сглаживание)": "TAA (antisserrilhamento nativo)",
    "Выключен": "Desativado",
    "Пресет": "Predefinição",
    "Резкость (RCAS)": "Nitidez (RCAS)",
    "Сила резкости": "Intensidade da nitidez",
    "Векторы движения объектов": "Vetores de movimento dos objetos",
    "Меньше гостинга на персонажах; стоит около 10% FPS": "Menos ghosting nos personagens; custa cerca de 10% de FPS",
    "Показывать FPS": "Mostrar FPS",
    "Ассеты найдены": "Recursos encontrados",
    "Нет ассетов: tools/fetch_fsr4_assets.sh": "Recursos ausentes: tools/fetch_fsr4_assets.sh",
    "Ассеты для выбранного режима найдены": "Recursos encontrados para o modo selecionado",
    "{}. Соберите FSR 4.1.1 из своей DLL кнопкой «Выбрать DLL…» ниже":
        "{}. Gere o FSR 4.1.1 a partir da sua própria DLL com o botão “Escolher DLL…” abaixo",
    "FSR 4.1.1 из своей DLL AMD": "FSR 4.1.1 da sua própria DLL da AMD",
    "Отменить": "Cancelar",
    "Выбрать DLL…": "Escolher DLL…",
    "DLL апскейлера AMD (amd_fidelityfx_upscaler_dx12.dll)": "DLL do upscaler da AMD (amd_fidelityfx_upscaler_dx12.dll)",
    "нет": "nenhum",
    "Эта DLL не подходит": "Esta DLL não serve",
    "{}: версия {}, модели FSR 4: {}.\n\nbbport повторяет только официальную FSR 4.1.x от AMD (модель v07_fp8_no_scale): например, FSR4_LATEST из OptiScaler или amd_fidelityfx_upscaler_dx12.dll из игры с FSR 4.1. Сборки сообщества (4.0.2b и подобные) устроены иначе и пока не поддерживаются.":
        "{}: versão {}, modelos do FSR 4: {}.\n\nO bbport reproduz apenas o FSR 4.1.x oficial da AMD (modelo v07_fp8_no_scale): por exemplo, o FSR4_LATEST do OptiScaler ou a amd_fidelityfx_upscaler_dx12.dll de um jogo com FSR 4.1. Builds da comunidade (4.0.2b e similares) têm outra estrutura e ainda não são suportados.",
    "Загрузчик не подходит": "O carregador não serve",
    "Загрузчик FidelityFX (amd_fidelityfx_loader_dx12.dll или amd_fidelityfx_dx12.dll)":
        "Carregador do FidelityFX (amd_fidelityfx_loader_dx12.dll ou amd_fidelityfx_dx12.dll)",
    "\n— сборка FSR 4.1.1 из {} —\n": "\n— gerando o FSR 4.1.1 a partir de {} —\n",
    "Подготовка…": "Preparando…",
    "Запись проходов DLL под Proton: {} из {}": "Gravando as passagens da DLL no Proton: {} de {}",
    "Перевод проходов в SPIR-V…": "Traduzindo as passagens para SPIR-V…",
    "— сборка FSR 4.1.1 завершилась (код {}) —\n": "— geração do FSR 4.1.1 concluída (código {}) —\n",
    "FSR 4.1.1 собран и выбран": "FSR 4.1.1 gerado e selecionado",
    "Сборка FSR 4.1.1 отменена": "Geração do FSR 4.1.1 cancelada",
    "FSR 4.1.1 не собран": "O FSR 4.1.1 não foi gerado",
    "DLL не подходит: подробности в журнале": "A DLL não serve: detalhes no log",
    "Не хватает программ для сборки: список в журнале": "Faltam programas para a geração: a lista está no log",
    "Записанные проходы не совпали с тем, что повторяет bbport (другая версия DLL или новая видеокарта?): подробности в журнале":
        "As passagens gravadas não batem com o que o bbport reproduz (outra versão da DLL ou GPU nova?): detalhes no log",
    "Не найден загрузчик FidelityFX 2.x": "Carregador do FidelityFX 2.x não encontrado",
    "Не найден подходящий Proton: установите GE-Proton 10 или новее (ProtonUp-Qt) или Proton Experimental / Proton-CachyOS в Steam. Если они есть — запустите в Steam любую игру с этим Proton, чтобы Steam поставил его рантайм":
        "Nenhum Proton adequado encontrado: instale o GE-Proton 10 ou mais novo (ProtonUp-Qt), ou o Proton Experimental / Proton-CachyOS na Steam. Se já os tiver, abra qualquer jogo com esse Proton na Steam para que a Steam instale o runtime dele",
    "Ни один Proton не запустил FSR 4.1 из этой DLL. Либо они слишком старые (подходят GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), либо DLL не включает FSR 4.1 на этой видеокарте (Steam Deck?): тогда соберите на ПК с Radeon RX 7000/9000 и скопируйте папку fsr4_411. Подробности в журнале":
        "Nenhum Proton iniciou o FSR 4.1 desta DLL. Ou eles são antigos demais (funcionam GE-Proton 10+, Proton Experimental, Proton-CachyOS 11), ou a DLL não ativa o FSR 4.1 nesta GPU (Steam Deck?): nesse caso, gere em um PC com Radeon RX 7000/9000 e copie a pasta fsr4_411. Detalhes no log",
    "На NixOS запись идёт на самой системе (systemd --user) через umu-launcher из nix-shell, а здесь их не нашлось: установите umu-launcher или Nix. Подробности в журнале":
        "No NixOS a gravação roda no próprio sistema (systemd --user) com o umu-launcher do nix-shell, e eles não foram encontrados: instale o umu-launcher ou o Nix. Detalhes no log",
    "{}: FSR {}. Это официальная FSR 4.0.x от AMD: она включается только на видеокартах RDNA4 (RX 9000), а под Proton на других видеокартах не запускается, поэтому записать её нельзя.\n\nНужна FSR 4.1.x: например, FSR4_LATEST из OptiScaler или DLL из игры с FSR 4.1.":
        "{}: FSR {}. Este é o FSR 4.0.x oficial da AMD: ele só é ativado em GPUs RDNA4 (RX 9000) e não inicia no Proton em outras GPUs, então não pode ser gravado.\n\nÉ preciso o FSR 4.1.x: por exemplo, o FSR4_LATEST do OptiScaler ou a DLL de um jogo com FSR 4.1.",
    "{}: в этой DLL нет FSR 4. Нужна amd_fidelityfx_upscaler_dx12.dll версии 4.1.x.":
        "{}: esta DLL não contém o FSR 4. É preciso a amd_fidelityfx_upscaler_dx12.dll versão 4.1.x.",
    "Это не загрузчик":
        "Este não é o carregador",
    "{} — это DLL апскейлера. Загрузчик называется amd_fidelityfx_loader_dx12.dll (у OptiScaler — amd_fidelityfx_dx12.dll) и лежит в той же папке игры, что и DLL апскейлера.":
        "{} é a DLL do upscaler. O carregador se chama amd_fidelityfx_loader_dx12.dll (amd_fidelityfx_dx12.dll no OptiScaler) e fica na mesma pasta do jogo que a DLL do upscaler.",
    "{}: версия {}. Нужен загрузчик FidelityFX 2.x — из той же игры, что и DLL апскейлера.":
        "{}: versão {}. É preciso um carregador do FidelityFX 2.x, do mesmo jogo que a DLL do upscaler.",
    "Рядом с DLL нет загрузчика: выберите amd_fidelityfx_loader_dx12.dll из папки игры":
        "Não há carregador ao lado da DLL: escolha a amd_fidelityfx_loader_dx12.dll da pasta do jogo",
    "Собрать FSR 4.1.1 из amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: папка FSR4_LATEST, или из игры с FSR 4.1). Нужен GE-Proton 10+, Proton Experimental или Proton-CachyOS; 2–5 минут (RDNA4: вдвое дольше, ещё и вариант FP8)":
        "Gerar o FSR 4.1.1 a partir de amd_fidelityfx_upscaler_dx12.dll 4.1.x (OptiScaler: pasta FSR4_LATEST, ou de um jogo com FSR 4.1). Requer GE-Proton 10+, Proton Experimental ou Proton-CachyOS; 2–5 minutos (RDNA4: o dobro, também com a variante FP8)",
    "Сборка не удалась (код {}): подробности в журнале": "A geração falhou (código {}): detalhes no log",
    "Сглаживание в разрешении вывода без модели FSR": "Antisserrilhamento na resolução de saída, sem modelo FSR",
    "Нет или повреждён файл {}": "Arquivo {} ausente ou corrompido",
    # Presets that are English source strings in the launcher
    "Native AA": "AA nativo",
    "Quality (x1.5)": "Qualidade (x1.5)",
    "Balanced (x1.7)": "Balanceado (x1.7)",
    "Performance (x2)": "Desempenho (x2)",
    "Ultra Performance (x3)": "Desempenho extremo (x3)",
    # Effects
    "Эффекты игры": "Efeitos do jogo",
    "Патчи игры, применяются при запуске": "Patches do jogo, aplicados ao iniciar",
    "Детализация моделей": "Detalhe dos modelos",
    "Как в игре": "Como no jogo",
    "Максимальная (-2)": "Máximo (-2)",
    "Ниже (1)": "Menor (1)",
    "Минимальная (2)": "Mínimo (2)",
    "Хроматическая аберрация": "Aberração cromática",
    "Глубина резкости (DoF)": "Profundidade de campo (DoF)",
    "Размытие в движении": "Desfoque de movimento",
    "Затенение SSAO": "SSAO",
    "Собственное сглаживание игры": "Antisserrilhamento nativo do jogo",
    "Тени от динамических источников": "Sombras de luzes dinâmicas",
    "Отражения SSR (не было в игре)": "Reflexos SSR (não existiam no jogo original)",
    "Пропуск заставок при запуске": "Pular vídeos de introdução",
    "Свободная камера (Cross + L3 / Space + Z)": "Câmera livre (Cross + L3 / Space + Z)",
    "Debug menu (левый touchpad / Tab; нужны шрифты)": "Menu de depuração (touchpad esquerdo / Tab; requer fontes)",
    "Установите DbgFont14h.ccm и DbgFont14h.tpf в dvdroot_ps4/font из мода Nexus #253":
        "Instale DbgFont14h.ccm e DbgFont14h.tpf em dvdroot_ps4/font a partir do mod #253 do Nexus",
    # Frame rate
    "Частота кадров": "Taxa de quadros",
    "Режим": "Modo",
    "Какой патч частоты кадров применить к игре": "Qual patch de taxa de quadros aplicar ao jogo",
    "Без ограничения (патч)": "Sem limite (patch)",
    "30 (как на PS4)": "30 (como no PS4)",
    "Ограничение FPS": "Limite de FPS",
    "0 — по частоте экрана (не выше 120 Гц); укажите число, чтобы ограничить иначе":
        "0: taxa de atualização da tela (até 120 Hz); defina um número para usar outro limite",
    # Performance
    "Производительность": "Desempenho",
    "Двухстадийный конвейер GPU": "Pipeline de GPU em dois estágios",
    "Быстрее на 20–30%; при нестабильности выключите": "20–30% mais rápido; desative se houver instabilidade",
    "Авто (8+ потоков)": "Automático (8+ threads)",
    "Включён": "Ativado",
    "Выключен (стабильнее)": "Desativado (mais estável)",
    "Чтение данных GPU процессором": "Leitura de dados da GPU pela CPU",
    "Relaxed (по умолчанию)": "Relaxed (padrão)",
    "Выключены": "Desativado",
    # Developer
    "Для разработчика": "Desenvolvedor",
    "Режим работы": "Modo",
    "Новая модель памяти и трансляции": "Novo modelo de memória e tradução",
    "Эксперимент, только видеокарты AMD. Видеокарта работает с памятью игры напрямую, как в игре для ПК, а команды графики переводятся, а не эмулируются; возможны ошибки. Выключено — старая модель памяти, как в 0.3, со всеми исправлениями":
        "Experimental, só GPUs AMD. A GPU usa a memória do jogo diretamente, como em um jogo de PC, e os comandos gráficos são traduzidos em vez de emulados; podem ocorrer erros. Desligado: o modelo de memória antigo, como na 0.3, com todas as correções",
    "Только для видеокарт AMD, а на этом компьютере её нет. Используется старая модель памяти, как в 0.3, со всеми исправлениями":
        "Só para GPUs AMD, e este computador não tem uma. É usado o modelo de memória antigo, como na 0.3, com todas as correções",
    "Синхронизация как в 0.3": "Sincronização como na 0.3",
    "Для поиска регрессий: старая модель памяти и ожидания копий как в релизе 0.3":
        "Para encontrar regressões: o modelo de memória antigo e as esperas de cópias como na versão 0.3",
    "Статистика кадров в журнале": "Estatísticas de quadros no log",
    "Профиль GPU в журнале": "Perfil da GPU no log",
    "Слои валидации Vulkan": "Camadas de validação do Vulkan",
    "Сильно замедляет": "Muito mais lento",
    "Доп. переменные (ИМЯ=значение через пробел)": "Variáveis extras (NOME=valor, separadas por espaço)",
}


def system_language():
    for key in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        value = os.environ.get(key, "")
        if value:
            locale = value.lower().replace("-", "_")
            if locale.startswith("ru"):
                return "ru"
            if locale.startswith("pt_br"):
                return "pt_BR"
            return "en"
    return "en"


_language = "ru"


def set_language(choice):
    """choice: "ru", "en", "pt_BR" or "" (the system's)."""
    global _language
    _language = choice if choice in ("ru", "en", "pt_BR") else system_language()


def language():
    return _language


def tr(text):
    if _language == "en":
        return EN.get(text, text)
    if _language == "pt_BR":
        # Strings added after the translation fall back to English, not to the Russian key.
        return PT_BR.get(text, EN.get(text, text))
    return text
