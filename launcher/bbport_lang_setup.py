# SPDX-License-Identifier: GPL-2.0-or-later
"""Translations of the setup guide, the System page and the hardware check (bbport_hardware).
Keyed by the English text like bbport_lang; merged into its tables on import. Languages without
an entry show the English text."""
import bbport_lang

SETUP = {'de': {
    # ---- hardware check (bbport.de/install) ----
    'Not recommended': 'Nicht empfohlen', 'Under 30 FPS': 'Unter 30 FPS', 'About 30 FPS': 'Etwa 30 FPS',
    '30–60 FPS': '30–60 FPS', '60–120 FPS': '60–120 FPS', '120+ FPS': '120+ FPS',
    'Not supported': 'Nicht unterstützt', 'Below minimum': 'Unter dem Minimum', 'Minimum': 'Minimum',
    'Above minimum': 'Über dem Minimum', 'Recommended': 'Empfohlen', '4K-ready': '4K-tauglich',
    "This PC can't run the port.": 'Auf diesem PC läuft der Port nicht.',
    'It may start, but expect low frame rates and problems.':
        'Er startet vielleicht, aber rechne mit niedrigen Bildraten und Problemen.',
    'Playable at 720p–1080p at about 30 FPS.': 'Spielbar in 720p–1080p mit etwa 30 FPS.',
    'Playable at 1080p; 1440p with upscaling.': 'Spielbar in 1080p; 1440p mit Upscaling.',
    'Good for 1440p at 60 FPS or more.': 'Gut für 1440p mit 60 FPS oder mehr.',
    'Ready for 4K with upscaling, and high frame rates at 1440p.':
        'Bereit für 4K mit Upscaling und hohe Bildraten in 1440p.',
    'Your graphics card sets the limit.': 'Deine Grafikkarte setzt die Grenze.',
    'Your processor sets the limit.': 'Dein Prozessor setzt die Grenze.',
    'Your memory sets the limit.': 'Dein Arbeitsspeicher setzt die Grenze.',
    'Upscaler off, lower details': 'Upscaler aus, weniger Details', 'Upscaler off': 'Upscaler aus',
    'Not supported: the port needs AVX, BMI1, MOVBE and LZCNT (Intel 4th gen or newer, or any Ryzen).':
        'Nicht unterstützt: Der Port braucht AVX, BMI1, MOVBE und LZCNT (Intel ab 4. Generation oder jeder Ryzen).',
    'Intel 2nd/3rd gen runs in compatibility mode, so expect bugs. It is also below the minimum (Core i3-10100).':
        'Intel 2./3. Generation läuft im Kompatibilitätsmodus, rechne mit Fehlern. Außerdem unter dem Minimum '
        '(Core i3-10100).',
    "Not in the website's list; it has every instruction the port needs. Counted as the minimum.":
        'Nicht in der Liste der Website, hat aber alle Befehle, die der Port braucht. Als Minimum gewertet.',
    'Below the minimum (Core i3-10100 / Ryzen 3 3100): expect low frame rates and stutter.':
        'Unter dem Minimum (Core i3-10100 / Ryzen 3 3100): rechne mit niedrigen Bildraten und Rucklern.',
    'Meets the minimum (Core i3-10100 / Ryzen 3 3100).': 'Erfüllt das Minimum (Core i3-10100 / Ryzen 3 3100).',
    'Above the minimum.': 'Über dem Minimum.',
    'Meets the recommendation (Ryzen 7 5700X / Core i7-12700).': 'Erfüllt die Empfehlung (Ryzen 7 5700X / Core i7-12700).',
    'Not supported: no Vulkan 1.3 driver for this card.': 'Nicht unterstützt: kein Vulkan-1.3-Treiber für diese Karte.',
    'Not supported: {} GB of video memory, the port needs at least 4 GB.':
        'Nicht unterstützt: {} GB Grafikspeicher, der Port braucht mindestens 4 GB.',
    'Too slow for the port.': 'Zu langsam für den Port.',
    'Below the minimum (GTX 1050 Ti / GTX 1650 / RX 570).': 'Unter dem Minimum (GTX 1050 Ti / GTX 1650 / RX 570).',
    'Meets the minimum (GTX 1050 Ti / GTX 1650 / RX 570 / RX 6500 XT).':
        'Erfüllt das Minimum (GTX 1050 Ti / GTX 1650 / RX 570 / RX 6500 XT).',
    'Meets the recommendation (RTX 3060 Ti / RTX 4060 / RX 6700 XT / RX 7600).':
        'Erfüllt die Empfehlung (RTX 3060 Ti / RTX 4060 / RX 6700 XT / RX 7600).',
    'Enough for 4K (RTX 4070 / RTX 5070 / RX 7800 XT / RX 9070 class).':
        'Reicht für 4K (Klasse RTX 4070 / RTX 5070 / RX 7800 XT / RX 9070).',
    'Integrated graphics use your RAM as video memory.': 'Integrierte Grafik nutzt den Arbeitsspeicher als Grafikspeicher.',
    '{} GB is not enough: the game will likely close right after PLAY (code 3).':
        '{} GB reichen nicht: Das Spiel schließt sich wahrscheinlich direkt nach SPIELEN (Code 3).',
    '{} GB is below the minimum of 16 GB: the game may close right after PLAY (code 3).':
        '{} GB liegen unter dem Minimum von 16 GB: Das Spiel kann sich direkt nach SPIELEN schließen (Code 3).',
    '{} GB meets the minimum (32 GB recommended). Keep the page file on "System managed".':
        '{} GB erfüllen das Minimum (32 GB empfohlen). Lass die Auslagerungsdatei auf „Vom System verwaltet“.',
    '{} GB meets the recommendation.': '{} GB erfüllen die Empfehlung.',
    'Settings for 4 GB cards: 720p/1080p, FSR 3.1 Performance, Model detail "As in the game", Object motion vectors off, '
    'frame generation off, a 60 FPS cap.':
        'Einstellungen für 4-GB-Karten: 720p/1080p, FSR 3.1 Performance, Modelldetail „Wie im Spiel“, '
        'Objekt-Bewegungsvektoren aus, Frame Generation aus, 60-FPS-Grenze.',
    'Intel graphics: keep the upscaler off, stretched polygons can still appear with it on (known issue in 1.9.2).':
        'Intel-Grafik: Lass den Upscaler aus, sonst können gestreckte Polygone auftreten (bekannter Fehler in 1.9.2).',
    'AMD driver 26.8.1: if the game crashes or shows broken graphics, clear the shader cache (Help) or try '
    'another driver.':
        'AMD-Treiber 26.8.1: Stürzt das Spiel ab oder zeigt Grafikfehler, leere den Shader-Cache (Hilfe) '
        'oder probiere einen anderen Treiber.',
    '{} is available on your card for even higher frame rates.': '{} ist auf deiner Karte für noch höhere Bildraten verfügbar.',
    'DLSS Frame Generation up to 4×': 'DLSS Frame Generation bis 4×', 'DLSS Frame Generation 2×': 'DLSS Frame Generation 2×',
    'FSR 3.1 frame generation works on your card; it works best from about 60 base FPS.':
        'FSR-3.1-Frame-Generation läuft auf deiner Karte; am besten ab etwa 60 Basis-FPS.',
    'Leave frame generation off: it needs about 60 base FPS and 12–16 GB of video memory to work well.':
        'Lass Frame Generation aus: Sie braucht etwa 60 Basis-FPS und 12–16 GB Grafikspeicher.',
    'Laptops: plug in the charger, set Windows to best performance, and make sure the game runs on the dedicated '
    'graphics card if you have one.':
        'Laptops: Netzteil anschließen, Windows auf „Beste Leistung“ stellen und das Spiel auf der dedizierten '
        'Grafikkarte laufen lassen, falls vorhanden.',
    'Integrated graphics share your RAM: 16 GB or more is a must.':
        'Integrierte Grafik teilt sich den Arbeitsspeicher: 16 GB oder mehr sind Pflicht.',
    # requirements
    'OS': 'Betriebssystem', 'CPU': 'Prozessor', 'RAM': 'Arbeitsspeicher', 'GPU': 'Grafikkarte', 'Storage': 'Speicher',
    'Windows 10 or 11, 64-bit': 'Windows 10 oder 11, 64 Bit', 'Windows 11, 64-bit': 'Windows 11, 64 Bit',
    'Intel Core i3-10100 or AMD Ryzen 3 3100': 'Intel Core i3-10100 oder AMD Ryzen 3 3100',
    'AMD Ryzen 7 5700X or Intel Core i7-12700': 'AMD Ryzen 7 5700X oder Intel Core i7-12700',
    '4 GB of video memory with Vulkan 1.3 (GTX 1050 Ti, GTX 1650, RX 570, RX 6500 XT)':
        '4 GB Grafikspeicher mit Vulkan 1.3 (GTX 1050 Ti, GTX 1650, RX 570, RX 6500 XT)',
    '8 GB of video memory or more (RTX 3060 Ti, RTX 4060, RX 6700 XT, RX 7600)':
        '8 GB Grafikspeicher oder mehr (RTX 3060 Ti, RTX 4060, RX 6700 XT, RX 7600)',
    'About 35 GB for your game files plus 1 GB for the port; an SSD is recommended':
        'Etwa 35 GB für deine Spieldateien plus 1 GB für den Port; eine SSD wird empfohlen',
    'SSD': 'SSD',
    'Minimum (720p–1080p, about 30 FPS)': 'Minimum (720p–1080p, etwa 30 FPS)',
    'Recommended (1440p, 60–120 FPS)': 'Empfohlen (1440p, 60–120 FPS)',
    'For 4K or frame generation: 12–16 GB of video memory (RTX 4070, RTX 5070, RX 7800 XT, RX 9070).':
        'Für 4K oder Frame Generation: 12–16 GB Grafikspeicher (RTX 4070, RTX 5070, RX 7800 XT, RX 9070).',
    'The CPU needs AVX, BMI1, MOVBE and LZCNT, which means Intel 4th gen (Haswell) or newer, or any Ryzen.':
        'Die CPU braucht AVX, BMI1, MOVBE und LZCNT, also Intel ab 4. Generation (Haswell) oder jeden Ryzen.',
    'Intel 2nd/3rd gen run in compatibility mode, so expect bugs.':
        'Intel 2./3. Generation läuft im Kompatibilitätsmodus, rechne mit Fehlern.',
    'CPUs without AVX (many Pentium/Celeron models) can’t run it at all.':
        'CPUs ohne AVX (viele Pentium-/Celeron-Modelle) können ihn gar nicht ausführen.',
    'DLSS needs a GeForce RTX 20 series or newer. DLSS Frame Generation needs RTX 40 (2×) or RTX 50 (up to 4×). '
    'FSR 3.1 frame generation works on any card and works best from about 60 base FPS.':
        'DLSS braucht eine GeForce RTX 20 oder neuer. DLSS Frame Generation braucht RTX 40 (2×) oder RTX 50 '
        '(bis 4×). FSR-3.1-Frame-Generation läuft auf jeder Karte, am besten ab etwa 60 Basis-FPS.',
    'FSR 4 is slow below Radeon RX 7000/9000; use FSR 3.1 there.':
        'FSR 4 ist unterhalb von Radeon RX 7000/9000 langsam; nimm dort FSR 3.1.',
    'AMD driver 26.8.1: if an RX 6000/9000 crashes or shows broken graphics, clear the shader cache (Help), '
    'or try another driver.':
        'AMD-Treiber 26.8.1: Stürzt eine RX 6000/9000 ab oder zeigt Grafikfehler, leere den Shader-Cache '
        '(Hilfe) oder probiere einen anderen Treiber.',
    'Intel graphics: stretched polygons can still appear with an upscaler on (known issue in 1.9.2), so turn it off.':
        'Intel-Grafik: Mit Upscaler können noch gestreckte Polygone auftreten (bekannter Fehler in 1.9.2), '
        'also schalte ihn aus.',
    'Overlays: if the game won’t start or keeps crashing, close MSI Afterburner, RTSS and other overlays.':
        'Overlays: Startet das Spiel nicht oder stürzt ab, schließe MSI Afterburner, RTSS und andere Overlays.',
    # ---- System page ----
    'System & performance': 'System & Leistung',
    'What your PC can do, detected automatically and rated like the hardware check on bbport.de. '
    'A rough guide, not a benchmark.':
        'Was dein PC schafft: automatisch erkannt und bewertet wie im Hardware-Check auf bbport.de. '
        'Eine grobe Einschätzung, kein Benchmark.',
    'System requirements': 'Systemanforderungen', 'Hard limits': 'Harte Grenzen', 'Driver notes': 'Treiber-Hinweise',
    'Your PC': 'Dein PC', 'Graphics card': 'Grafikkarte', 'Processor': 'Prozessor', 'Memory': 'Arbeitsspeicher',
    'Detected automatically': 'Automatisch erkannt', 'Unknown processor': 'Unbekannter Prozessor',
    '(chosen by hand)': '(von Hand gewählt)', 'Wrong graphics card?': 'Falsche Grafikkarte?',
    'Checking your PC…': 'Dein PC wird geprüft…',
    'Your graphics card ({}) is not in the list of the hardware check. Pick it or the closest model:':
        'Deine Grafikkarte ({}) steht nicht in der Liste des Hardware-Checks. Wähle sie oder das ähnlichste Modell:',
    'Use the recommended settings': 'Empfohlene Einstellungen übernehmen', 'Start the setup guide': 'Einrichtung starten',
    'Copy system info': 'Systeminfo kopieren',
    'Copied: paste it into your message on Discord or in the forum.':
        'Kopiert: Füge es in deine Nachricht auf Discord oder im Forum ein.',
    'Set: {} with {}.': 'Übernommen: {} mit {}.', '{} GB free on {}': '{} GB frei auf {}',
    'Install guide and FAQ on bbport.de': 'Installationsanleitung und FAQ auf bbport.de',
    'Expected: {} at {}': 'Erwartet: {} bei {}', 'Expected': 'Erwartet',
    'Performance: {} · {}': 'Leistung: {} · {}',
    'Performance: pick your graphics card on System & performance':
        'Leistung: Wähle deine Grafikkarte unter System & Leistung',
    'Performance: checking your PC…': 'Leistung: Dein PC wird geprüft…',
    # ---- setup guide ----
    'Awakening': 'Erwachen', 'Welcome': 'Willkommen', 'Blood test': 'Blutprobe', 'Yharnam': 'Yharnam',
    "Hunter's sight": 'Jägerblick', 'The hunt': 'Die Jagd', 'Ready to play': 'Bereit zum Spielen',
    'FIRST AWAKENING': 'ERSTES ERWACHEN', 'Setup': 'Einrichtung', 'Step {} of {}': 'Schritt {} von {}',
    'Next': 'Weiter', 'Back': 'Zurück', 'Skip: I set everything up myself': 'Überspringen: Ich richte alles selbst ein',
    'Welcome, good hunter': 'Willkommen, Jäger',
    'A few steps to your first hunt. Every setting can be changed in the launcher afterwards, just as before.':
        'Wenige Schritte bis zur ersten Jagd. Jede Einstellung lässt sich danach im Launcher ändern, genau wie bisher.',
    'This guide checks your PC, finds your game and picks graphics settings that suit your hardware, in about two '
    'minutes. Nothing is hidden: every page of the launcher is still there to change each setting by hand.':
        'Diese Einrichtung prüft deinen PC, findet dein Spiel und wählt passende Grafikeinstellungen für deine '
        'Hardware, in etwa zwei Minuten. Nichts wird versteckt: Jede Seite des Launchers bleibt, um alles von Hand '
        'einzustellen.',
    'What you need': 'Was du brauchst',
    'Your own Bloodborne game files (CUSA03173, Game of the Year Edition) with the 1.09 update.':
        'Deine eigenen Bloodborne-Spieldateien (CUSA03173, Game of the Year Edition) mit Update 1.09.',
    'Windows 10 or 11 (64-bit) and a graphics card with Vulkan 1.3 and at least 4 GB of video memory.':
        'Windows 10 oder 11 (64 Bit) und eine Grafikkarte mit Vulkan 1.3 und mindestens 4 GB Grafikspeicher.',
    'About 35 GB for the game files and 1 GB for the port, best on an SSD.':
        'Etwa 35 GB für die Spieldateien und 1 GB für den Port, am besten auf einer SSD.',
    'No game files are included or shared, and we do not help find them.':
        'Es werden keine Spieldateien mitgeliefert oder geteilt, und wir helfen nicht, welche zu finden.',
    'Apply (restarts the launcher)': 'Übernehmen (startet den Launcher neu)',
    'Detected automatically and rated like the hardware check on bbport.de.':
        'Automatisch erkannt und bewertet wie im Hardware-Check auf bbport.de.',
    'Taking a blood sample…': 'Blutprobe wird entnommen…',
    'This PC does not meet the requirements. You can still go on, but the game will probably not run.':
        'Dieser PC erfüllt die Anforderungen nicht. Du kannst trotzdem weitermachen, aber das Spiel läuft '
        'wahrscheinlich nicht.',
    'Your game folder': 'Dein Spielordner',
    'Your own Bloodborne game files with the 1.09 update. Nothing leaves your PC.':
        'Deine eigenen Bloodborne-Spieldateien mit Update 1.09. Nichts verlässt deinen PC.',
    'Choose your game folder: the one with eboot.bin, sce_module, sce_sys and dvdroot_ps4 directly inside.':
        'Wähle deinen Spielordner: den, in dem eboot.bin, sce_module, sce_sys und dvdroot_ps4 direkt liegen.',
    'Found automatically: {}': 'Automatisch gefunden: {}',
    'Used the subfolder {} (it has eboot.bin).': 'Unterordner {} verwendet (dort liegt eboot.bin).',
    'Check': 'Prüfung',
    'No folder chosen yet. Choose your game folder: the one with eboot.bin directly inside.':
        'Noch kein Ordner gewählt. Wähle deinen Spielordner: den, in dem eboot.bin direkt liegt.',
    'There is no eboot.bin in this folder. Choose the folder that has eboot.bin directly inside, not the one '
    'above it.':
        'In diesem Ordner liegt kein eboot.bin. Wähle den Ordner, in dem eboot.bin direkt liegt, nicht den darüber.',
    'eboot.bin found.': 'eboot.bin gefunden.',
    'CUSA03173: the Game of the Year Edition, with The Old Hunters.':
        'CUSA03173: die Game of the Year Edition, mit The Old Hunters.',
    '{}: the port is made for CUSA03173, the Game of the Year Edition. Other editions may not work.':
        '{}: Der Port ist für CUSA03173 gemacht, die Game of the Year Edition. Andere Ausgaben laufen '
        'möglicherweise nicht.',
    'Version 01.09: every community patch is available.': 'Version 01.09: Alle Community-Patches sind verfügbar.',
    'Version {}: the game runs at 30 FPS only. Copy everything from the 1.09 update into this folder and replace '
    'the files.':
        'Version {}: Das Spiel läuft nur mit 30 FPS. Kopiere alles aus dem Update 1.09 in diesen Ordner und ersetze '
        'die Dateien.',
    'Missing: {}. Unpack the base game completely into this folder.':
        'Es fehlt: {}. Entpacke das Basisspiel vollständig in diesen Ordner.',
    'sce_module, sce_sys and dvdroot_ps4 are there.': 'sce_module, sce_sys und dvdroot_ps4 sind vorhanden.',
    'The port is in {}: synced folders like OneDrive and Program Files cause problems. Move it to a plain folder '
    'like C:\\Games\\BBPort.':
        'Der Port liegt in {}: Synchronisierte Ordner wie OneDrive und der Programme-Ordner machen Probleme. '
        'Verschiebe ihn in einen einfachen Ordner wie C:\\Games\\BBPort.',
    'How to prepare the game folder (once)': 'So bereitest du den Spielordner vor (einmalig)',
    'Put the base game and the 1.09 update into two new, empty folders.':
        'Lege das Grundspiel und das Update 1.09 in zwei neue, leere Ordner.',
    'Copy everything from the update folder into the base game folder and choose "Replace the files in the '
    'destination".':
        'Kopiere alles aus dem Update-Ordner in den Ordner des Basisspiels und wähle „Dateien im Ziel ersetzen“.',
    'Check the result: eboot.bin (about 89 MB), sce_module, sce_sys and dvdroot_ps4 (about 29 GB) are directly '
    'inside. The Old Hunters is already part of the GOTY edition.':
        'Prüfe das Ergebnis: eboot.bin (etwa 89 MB), sce_module, sce_sys und dvdroot_ps4 (etwa 29 GB) liegen direkt '
        'darin. The Old Hunters ist in der GOTY-Edition schon enthalten.',
    'Graphics for your PC': 'Grafik für deinen PC',
    'Pick a resolution: the expected frame rate comes from your hardware. Fine-tune it later on the Graphics pages.':
        'Wähle eine Auflösung: Die erwartete Bildrate ergibt sich aus deiner Hardware. Feinheiten später auf den '
        'Grafik-Seiten.',
    'Your display: {} × {}. Recommended for your PC: {}.': 'Dein Bildschirm: {} × {}. Empfohlen für deinen PC: {}.',
    'RECOMMENDED': 'EMPFOHLEN', 'Options': 'Optionen',
    '{} frame generation: higher frame rates, a little more input latency':
        '{}-Frame-Generation: höhere Bildraten, etwas mehr Eingabeverzögerung',
    'Download the FSR 4 files now (about 30 MB)': 'FSR-4-Dateien jetzt herunterladen (etwa 30 MB)',
    'More on Graphics, Display & FPS and Game & effects: upscaler, quality, frame cap, effects, field of view.':
        'Mehr unter Grafik, Anzeige & FPS und Spiel & Effekte: Upscaler, Qualität, FPS-Grenze, Effekte, Sichtfeld.',
    'Your graphics card is not rated yet (see the previous step), so your current graphics settings stay as they '
    'are.':
        'Deine Grafikkarte ist noch nicht bewertet (siehe vorheriger Schritt), deshalb bleiben deine '
        'Grafikeinstellungen, wie sie sind.',
    'The hunt begins': 'Die Jagd beginnt',
    'Everything is set. Press PLAY, or look around the launcher first.':
        'Alles ist eingerichtet. Drück SPIELEN oder sieh dich zuerst im Launcher um.',
    'Your setup': 'Deine Einrichtung', 'not set': 'nicht gesetzt', '{} · version {}': '{} · Version {}',
    'Extras': 'Extras', 'Window': 'Fenster',
    'Online needs a free shadNet account: sign in on the Online co-op page.':
        'Online braucht ein kostenloses shadNet-Konto: Melde dich auf der Seite Online-Koop an.',
    'The first minutes can stutter while the shader cache is built. That is normal.':
        'Die ersten Minuten können ruckeln, während der Shader-Cache entsteht. Das ist normal.',
    "In the game, Insert (keyboard) or L3 + R3 (controller) opens the port's menu.":
        'Im Spiel öffnet Einfg (Tastatur) oder L3 + R3 (Controller) das Menü des Ports.',
    'Next time, Play Bloodborne.exe starts the game straight away with these settings.':
        'Beim nächsten Mal startet Play Bloodborne.exe das Spiel direkt mit diesen Einstellungen.',
    # ---- saves from other versions ----
    'Saves from other versions': 'Spielstände aus anderen Versionen',
    "Characters from shadPS4, Awaken's port, bbhost or a PS4 export: found on this PC and copied into this port's "
    "saves folder. Your current saves are backed up first.":
        'Charaktere aus shadPS4, Awakens Port, bbhost oder einem PS4-Export: auf diesem PC gefunden und in den '
        'Spielstand-Ordner dieses Ports kopiert. Deine aktuellen Spielstände werden vorher gesichert.',
    'Played before?': 'Schon woanders gespielt?',
    'This port: {}, last played {}': 'Dieser Port: {}, zuletzt gespielt {}',
    'This port has no saves yet.': 'Dieser Port hat noch keine Spielstände.',
    "Searching this PC for saves of shadPS4, Awaken's port and bbhost…":
        'Suche auf diesem PC nach Spielständen von shadPS4, Awakens Port und bbhost…',
    "No saves from shadPS4, Awaken's port or bbhost found on this PC. If yours are somewhere else (a backup, a PS4 "
    "export), choose their folder.":
        'Keine Spielstände von shadPS4, Awakens Port oder bbhost auf diesem PC gefunden. Liegen deine woanders '
        '(ein Backup, ein PS4-Export), wähle ihren Ordner.',
    'no characters': 'keine Charaktere', '1 character': '1 Charakter', '{} characters': '{} Charaktere',
    'last played {}': 'zuletzt gespielt {}', 'Import': 'Importieren', 'Search again': 'Erneut suchen',
    'Choose a folder…': 'Ordner wählen…', 'Backups': 'Sicherungen',
    'Choose the folder with the saves': 'Ordner mit den Spielständen wählen',
    "These are a PS4's own encrypted backups (sdimg_…); they cannot be used. Export the save with Apollo Save Tool "
    "on the PS4 instead.":
        'Das sind verschlüsselte Sicherungen einer PS4 (sdimg_…); die lassen sich nicht verwenden. Exportiere den '
        'Spielstand stattdessen mit dem Apollo Save Tool auf der PS4.',
    'No complete Bloodborne save in {} or its subfolders: it needs userdata0010 and at least one character '
    '(userdata0000 to userdata0009).':
        'Kein vollständiger Bloodborne-Spielstand in {} oder seinen Unterordnern: Er braucht userdata0010 und '
        'mindestens einen Charakter (userdata0000 bis userdata0009).',
    'Close the game first; it writes its saves while it runs.':
        'Schließe zuerst das Spiel; es schreibt seine Spielstände, solange es läuft.',
    'Import the saves from {} ({}, last played {})?': 'Die Spielstände aus {} importieren ({}, zuletzt gespielt {})?',
    "They replace this port's saves ({}). Those are packed into {} first, so nothing is lost.":
        'Sie ersetzen die Spielstände dieses Ports ({}). Die werden vorher nach {} gesichert, es geht also nichts '
        'verloren.',
    'Could not import the saves: {}': 'Die Spielstände konnten nicht importiert werden: {}',
    'Your achievements from there came along: {} new here.':
        'Deine Erfolge von dort sind mitgekommen: {} neu hier.',
    "Saves imported. Your characters are in the game's Load Game menu.":
        'Spielstände importiert. Deine Charaktere findest du im Spiel unter „Spiel laden“.',
    'Your previous saves: {}': 'Deine bisherigen Spielstände: {}',
    # ---- automatic settings ----
    'Settings for your PC': 'Einstellungen für deinen PC',
    'All settings match the recommendation.': 'Alle Einstellungen entsprechen der Empfehlung.',
    '1 setting differs from the recommendation.': '1 Einstellung weicht von der Empfehlung ab.',
    '{} settings differ from the recommendation.': '{} Einstellungen weichen von der Empfehlung ab.',
    'Derived from the requirements and the hardware check on bbport.de for your PC at {} ({}). Chromatic '
    'aberration, depth of field, motion blur and sharpening are taste and stay as you set them.':
        'Abgeleitet aus den Anforderungen und dem Hardware-Check auf bbport.de für deinen PC bei {} ({}). '
        'Chromatische Aberration, Tiefenschärfe, Bewegungsunschärfe und Schärfe sind Geschmackssache und bleiben, '
        'wie du sie eingestellt hast.',
    'Setting': 'Einstellung', 'Now': 'Jetzt', 'For your PC': 'Für deinen PC', 'Why': 'Warum',
    'Apply the settings for my PC': 'Einstellungen für meinen PC übernehmen', 'Undo': 'Rückgängig',
    'Adjust them automatically when the hardware changes': 'Automatisch anpassen, wenn sich die Hardware ändert',
    'New hardware detected: the graphics settings were adjusted to it.':
        'Neue Hardware erkannt: Die Grafikeinstellungen wurden angepasst.',
    'Settings: made for your PC': 'Einstellungen: passend für deinen PC',
    'Settings: adjusted to your new hardware (Undo on System & performance)':
        'Einstellungen: an deine neue Hardware angepasst (Rückgängig unter System & Leistung)',
    'Settings: 1 differs from the recommendation for your PC (System & performance)':
        'Einstellungen: 1 weicht von der Empfehlung für deinen PC ab (System & Leistung)',
    'Settings: {} differ from the recommendation for your PC (System & performance)':
        'Einstellungen: {} weichen von der Empfehlung für deinen PC ab (System & Leistung)',
    'Set for your PC as well: ambient occlusion, shadows, model detail, motion vectors, anti-aliasing and sharpening '
    '(see System & performance). Everything can be changed on the Graphics pages.':
        'Ebenfalls für deinen PC eingestellt: Umgebungsverdeckung, Schatten, Modelldetail, Bewegungsvektoren, '
        'Kantenglättung und Schärfe (siehe System & Leistung). Alles lässt sich auf den Grafik-Seiten ändern.',
    'Frame generation': 'Frame Generation',
    'The highest resolution up to your display at which your PC is expected to reach 60 FPS or more (else 30–60, '
    'else 1080p).':
        'Die höchste Auflösung bis zu deinem Bildschirm, bei der dein PC voraussichtlich 60 FPS oder mehr schafft '
        '(sonst 30–60, sonst 1080p).',
    'Below the minimum at 1080p: 720p, the low end of the minimum requirements.':
        'Unter dem Minimum bei 1080p: 720p, das untere Ende der Mindestanforderungen.',
    "The website's settings for 4 GB cards: FSR 3.1.": 'Die Website-Einstellungen für 4-GB-Karten: FSR 3.1.',
    "The website's settings for 4 GB cards: Performance.": 'Die Website-Einstellungen für 4-GB-Karten: Performance.',
    "The website's settings for 4 GB cards: a 60 FPS cap.": 'Die Website-Einstellungen für 4-GB-Karten: 60-FPS-Grenze.',
    "The website's settings for 4 GB cards: model detail as in the game.":
        'Die Website-Einstellungen für 4-GB-Karten: Modelldetail wie im Spiel.',
    'Intel graphics: stretched polygons can still appear with an upscaler on.':
        'Intel-Grafik: Mit Upscaler können noch gestreckte Polygone auftreten.',
    'DLSS on a GeForce RTX.': 'DLSS auf einer GeForce RTX.',
    'FSR 4 on a Radeon RX 7000/9000.': 'FSR 4 auf einer Radeon RX 7000/9000.',
    'FSR 3.1: FSR 4 is slow below the Radeon RX 7000/9000.': 'FSR 3.1: FSR 4 ist unterhalb der Radeon RX 7000/9000 langsam.',
    'Balanced: Performance would render 720p from only 360 lines.':
        'Balanced: Performance würde 720p aus nur 360 Zeilen berechnen.',
    "The website's suggestion for the expected frame rate.": 'Der Vorschlag der Website für die erwartete Bildrate.',
    'Unlocked, capped at the display refresh (at most 120): your PC is expected above 60 FPS.':
        'Unbegrenzt, gedeckelt auf die Bildwiederholrate (höchstens 120): Dein PC schafft voraussichtlich über 60 FPS.',
    'A steady cap at what your PC is expected to hold.': 'Eine feste Grenze, die dein PC voraussichtlich hält.',
    "Auto: the display refresh, at most 120 (the game's timing breaks above).":
        'Auto: die Bildwiederholrate, höchstens 120 (darüber gerät das Timing des Spiels durcheinander).',
    "The game's own anti-aliasing only without an upscaler: the upscaler smooths edges itself.":
        'Die Kantenglättung des Spiels nur ohne Upscaler: Der Upscaler glättet die Kanten selbst.',
    'Ambient occlusion costs GPU time: on from "Above minimum".':
        'Umgebungsverdeckung kostet GPU-Zeit: an ab „Über dem Minimum“.',
    'Shadows of dynamic lights: on from "Above minimum".': 'Schatten dynamischer Lichter: an ab „Über dem Minimum“.',
    'Screen-space reflections are expensive: off below "4K-ready" headroom.':
        'Screen-Space-Reflexionen sind teuer: aus unterhalb von „4K-tauglich“.',
    'Highest model detail: your card has room to spare at this resolution.':
        'Höchstes Modelldetail: Deine Karte hat bei dieser Auflösung Reserven.',
    'Model detail as in the game.': 'Modelldetail wie im Spiel.',
    'Lower model detail: below "Above minimum" every millisecond counts.':
        'Geringeres Modelldetail: Unter „Über dem Minimum“ zählt jede Millisekunde.',
    'Object motion vectors cost about 5–10% FPS: on from "Recommended".':
        'Objekt-Bewegungsvektoren kosten etwa 5–10 % FPS: an ab „Empfohlen“.',
    'RCAS sharpening with an upscaler.': 'RCAS-Schärfung mit Upscaler.',
    'DLSS is sharper by itself; FSR gets 0.50.': 'DLSS ist von sich aus schärfer; FSR bekommt 0,50.',
    'Frame generation needs about 60 base FPS and 12–16 GB of video memory.':
        'Frame Generation braucht etwa 60 Basis-FPS und 12–16 GB Grafikspeicher.',
}}

for _language, _texts in SETUP.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(_texts)
