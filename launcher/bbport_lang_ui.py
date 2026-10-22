# SPDX-License-Identifier: GPL-2.0-or-later
"""German texts of the launcher's pages that had none: the regrouped pages (Picture &
performance, Saves, Game & language, All graphics, System & tests), the side bar and its search,
the guide's new steps, and the older pages whose German was missing (Controls, Online play,
Achievements, Advanced). Keyed by the English text like bbport_lang; merged into its tables on
import. Languages without an entry show the English text."""
import bbport_lang

UI = {'de': {
    # ---- side bar, search, view ----
    'Picture & performance': 'Bild & Leistung', 'Online play': 'Online spielen', 'Saves': 'Spielstände',
    'Mods': 'Mods', 'Game & language': 'Spiel & Sprache', 'Achievements': 'Erfolge', 'All graphics': 'Grafik (alle)',
    'System & tests': 'System & Tests', 'EXPERT': 'EXPERTE', 'Expert view': 'Expertenmodus', 'Search…': 'Suchen…',
    'problem': 'Problem', 'Ctrl+F': 'Strg+F', 'Performance: {}': 'Leistung: {}', 'Page': 'Seite', 'expert view': 'Expertenmodus',
    'Nothing found. Try other words, or open Help.': 'Nichts gefunden. Versuch andere Wörter oder öffne die Hilfe.',
    'co-op test build': 'Koop-Testversion',
    # ---- Play ----
    'Your goal': 'Dein Ziel', 'Smooth (goal)': 'Flüssig', 'Balanced (goal)': 'Ausgewogen', 'Quiet (goal)': 'Leise',
    'As many frames as the game allows, up to 120 FPS; the picture made for your PC.':
        'So viele Bilder, wie das Spiel erlaubt, bis 120 FPS; das Bild passend zu deinem PC.',
    'A steady 60 FPS with the best picture your graphics card has room for.':
        'Stabile 60 FPS mit dem besten Bild, für das deine Grafikkarte Luft hat.',
    '60 FPS and a little less detail: the graphics card works less, stays cooler and quieter (laptops).':
        '60 FPS und etwas weniger Details: Die Grafikkarte arbeitet weniger, bleibt kühler und leiser (Laptops).',
    '• No saves yet: bring your character from another version, or the game creates them':
        '• Noch keine Spielstände: Bring deinen Charakter aus einer anderen Version mit, oder das Spiel legt sie an',
    '• FSR 4 files missing: FSR 3.1 is used meanwhile. Click to download (30 MB)':
        '• FSR-4-Dateien fehlen: Solange wird FSR 3.1 genutzt. Klick zum Herunterladen (30 MB)',
    'Online is on, but you are not signed in yet: sign in now':
        'Online ist an, aber du bist noch nicht angemeldet: jetzt anmelden',
    'not signed in ›': 'nicht angemeldet ›', 'signed in as {}': 'angemeldet als {}', 'Cheats on: {}': 'Cheats an: {}',
    'New in version {}: what has changed': 'Neu in Version {}: was sich geändert hat',
    "Online (The Hunter's Dream)": "Online (The Hunter's Dream)", 'Online (SuperMedo Server)': 'Online (SuperMedo Server)',
    'Sign in now? (No: play offline this time.)': 'Jetzt anmelden? (Nein: diesmal offline spielen.)',
    "To play online with The Hunter's Dream, sign in with Discord first.":
        "Um online mit The Hunter's Dream zu spielen, melde dich zuerst mit Discord an.",
    'To play on SuperMedo Server, enter your supermedo.com username and password on the Online co-op page (Sign up '
    'there if you have no account).':
        'Für den SuperMedo Server gib auf der Seite „Online spielen“ deinen supermedo.com-Benutzernamen und dein Passwort '
        'ein (dort kannst du dich auch registrieren).',
    'Your eboot.bin is not the clean 1.09 one (modified, or from another release). The port cannot patch it: '
    'the game runs at 30 FPS, the Game & effects options do nothing, and some scenes (like the cutscene '
    'after Rom) can freeze.\n\nReplace eboot.bin with the one from an unmodified 1.09 update.':
        'Deine eboot.bin ist nicht die saubere 1.09 (verändert oder aus einer anderen Ausgabe). Der Port '
        'kann sie nicht patchen: Das Spiel läuft mit 30 FPS, die Effekt-Optionen wirken nicht, und manche '
        'Szenen (etwa die Zwischensequenz nach Rom) können einfrieren.\n\nErsetze die eboot.bin durch die '
        'aus einem unveränderten 1.09-Update (Hilfe zeigt wie).',
    # ---- Picture & performance ----
    'The launcher sets the technical settings for your PC. You choose how it should feel; the in-game menu (Insert or '
    'L3+R3) changes the same values.':
        'Der Launcher stellt die Technik für deinen PC ein. Du wählst, wie es sich anfühlen soll; das Menü im Spiel '
        '(Einfg oder L3+R3) ändert dieselben Werte.',
    'Made for your PC': 'Für deinen PC eingestellt', 'as recommended': 'wie empfohlen',
    '{} differ from the recommendation': '{} weichen von der Empfehlung ab', 'checking your PC…': 'prüfe deinen PC…',
    'every effect': 'alle Effekte', 'fewer effects': 'weniger Effekte', 'no costly effects': 'ohne teure Effekte',
    'Your PC: {} · {}': 'Dein PC: {} · {}', 'Measure on this PC': 'Auf diesem PC messen',
    'Performance test': 'Leistungstest', 'Show the settings one by one': 'Einstellungen einzeln zeigen',
    'Hide the settings': 'Einstellungen ausblenden',
    'Last measured on {}: {:.0f} FPS (1 % low {:.0f}) for {} FPS  ›  report':
        'Zuletzt gemessen am {}: {:.0f} FPS (1 %-Low {:.0f}) für {} FPS  ›  Bericht',
    'Measuring takes 2 to 10 minutes: the game starts a few times by itself from your last save, on a copy of it.':
        'Messen dauert 2 bis 10 Minuten: Das Spiel startet ein paar Mal von selbst von deinem letzten Spielstand, mit '
        'einer Kopie davon.',
    'Screen': 'Bildschirm', 'Taste': 'Geschmack', 'Field of view': 'Sichtfeld',
    'Recommended: the game gets the display to itself (smoother, and 4K downsampling works).':
        'Empfohlen: Das Spiel hat den Bildschirm für sich (flüssiger, und 4K-Downsampling funktioniert).',
    'Vertical, in degrees. Every camera of the game is widened by the same amount.':
        'Vertikal, in Grad. Jede Kamera des Spiels wird um denselben Betrag erweitert.',
    'Every graphics setting one by one:': 'Jede Grafikeinstellung einzeln:', 'Open': 'Öffnen',
    'A game test is running already.': 'Es läuft schon ein Spieltest.',
    'Settings: adjusted to your new hardware (Undo on Picture & performance)':
        'Einstellungen: an deine neue Hardware angepasst (Rückgängig unter Bild & Leistung)',
    'Settings: made for your PC and your goal': 'Einstellungen: passend zu deinem PC und deinem Ziel',
    'Settings: 1 differs from the recommendation for your PC': 'Einstellungen: 1 weicht von der Empfehlung ab',
    'Settings: {} differ from the recommendation for your PC': 'Einstellungen: {} weichen von der Empfehlung ab',
    'Performance: pick your graphics card (System & tests)': 'Leistung: Wähle deine Grafikkarte (System & Tests)',
    # goals in the profile's reasons
    'Goal "Balanced": at 60 FPS your card has room for a sharper picture, one quality step up.':
        'Ziel „Ausgewogen“: Bei 60 FPS hat deine Karte Luft für ein schärferes Bild, eine Qualitätsstufe höher.',
    'Goal "Quiet": one quality step down, less work for the graphics card.':
        'Ziel „Leise“: eine Qualitätsstufe niedriger, weniger Arbeit für die Grafikkarte.',
    'Goal "Balanced": a steady 60 FPS.': 'Ziel „Ausgewogen“: stabile 60 FPS.',
    'Goal "Quiet": 60 FPS; the card rests between frames.': 'Ziel „Leise“: 60 FPS; die Karte ruht zwischen den Bildern.',
    'Goal "Quiet": model detail as in the game.': 'Ziel „Leise“: Modelldetail wie im Spiel.',
    'Goal "Quiet": object motion vectors off (they cost about 5–10% FPS).':
        'Ziel „Leise“: Objekt-Bewegungsvektoren aus (sie kosten etwa 5–10 % FPS).',
    # ---- Saves ----
    'Your characters: bring them from another version, back them up, put a backup back.':
        'Deine Charaktere: aus einer anderen Version mitbringen, sichern, eine Sicherung zurückspielen.',
    # ---- Game & language ----
    'Your game files, their languages and how the game starts.': 'Deine Spieldateien, ihre Sprachen und wie das Spiel '
                                                        'startet.',
    'Is the game folder all right? Help checks it.': 'Ist der Spielordner in Ordnung? Die Hilfe prüft ihn.',
    'Language': 'Sprache', 'Text language': 'Textsprache', 'Voice language': 'Sprachausgabe',
    'As in the game uses the original voice setting; other choices use available tracks.':
        '„Wie im Spiel“ nutzt die Spieleinstellung; die anderen nutzen die vorhandenen Tonspuren.',
    'Start and sound': 'Start und Ton', 'Mute when the game is in the background': 'Stumm, wenn das Spiel im Hintergrund ist',
    # ---- All graphics ----
    'Every graphics and frame rate setting. Stored in bbport.ini; the in-game menu (Insert or L3+R3) changes the same '
    'values.':
        'Jede Grafik- und Bildrateneinstellung. Gespeichert in bbport.ini; das Menü im Spiel (Einfg oder L3+R3) ändert '
        'dieselben Werte.',
    'Fullscreen, HDR, field of view and the taste effects are on Picture & performance.':
        'Vollbild, HDR, Sichtfeld und die Geschmackseffekte sind unter Bild & Leistung.',
    "Temporal upscaling with the game's own motion vectors. FSR 4 needs its files (below) and a GPU with INT8 dot "
    'products; otherwise the game falls back to FSR 3.1 by itself.':
        'Zeitliche Hochskalierung mit den Bewegungsvektoren des Spiels. FSR 4 braucht seine Dateien (unten) und eine GPU '
        'mit INT8; sonst wechselt das Spiel selbst zu FSR 3.1.',
    'HUD position (ultrawide)': 'HUD-Position (Ultrawide)',
    '21:9 outputs: health, items and Blood Echoes at the screen edges, or in the centred 16:9 area as on a 16:9 screen.':
        '21:9: Leben, Gegenstände und Blutechos an den Bildschirmrändern oder im mittleren 16:9-Bereich wie auf einem '
        '16:9-Bildschirm.',
    'Less ghosting on characters, cloth and weapons; costs about 5–10% FPS.':
        'Weniger Nachziehen bei Figuren, Kleidung und Waffen; kostet etwa 5–10 % FPS.',
    'Effects are game patches for version 1.09, applied at start.': 'Effekte sind Spiel-Patches für Version 1.09 und gelten beim Start.',
    "Frames made between two of the game's: 60 FPS on screen becomes 120. Needs a display with that refresh rate (120 "
    'Hz or more at 60 FPS); it pauses when the display cannot show the extra frames. Adds a little input latency. Menus '
    'and movies are not generated. DLSS needs a GeForce RTX 40 or newer, otherwise FSR 3.1 is used.':
        'Bilder zwischen zwei Bildern des Spiels: Aus 60 FPS werden 120. Braucht einen Bildschirm mit dieser '
        'Bildwiederholrate (120 Hz oder mehr bei 60 FPS); sie pausiert, wenn er die zusätzlichen Bilder nicht zeigen '
        'kann. Etwas mehr Eingabeverzögerung. Menüs und Videos werden nicht erzeugt. DLSS braucht eine GeForce RTX 40 '
        'oder neuer, sonst wird FSR 3.1 genutzt.',
    'Multiplier': 'Faktor', 'Fit to display refresh': 'An die Bildwiederholrate anpassen',
    'FSR 3.1 makes one frame (2×); DLSS on a GeForce RTX 50 up to three (4×).':
        'FSR 3.1 erzeugt ein Bild (2×); DLSS auf einer GeForce RTX 50 bis zu drei (4×).',
    'On: no more frames than the display shows (an uncapped game gets fewer generated frames, and the GPU does less). '
    'Off: the multiplier as chosen.':
        'An: nicht mehr Bilder, als der Bildschirm zeigt (ein unbegrenztes Spiel bekommt weniger erzeugte Bilder, die '
        'GPU arbeitet weniger). Aus: der Faktor wie gewählt.',
    'Game (43°)': 'Wie im Spiel (43°)', 'Screen edges': 'Bildschirmränder', 'Halfway': 'Halber Weg',
    'Centred (16:9)': 'Mittig (16:9)', 'AMD FSR 3.1 Frame Generation (any GPU)': 'AMD FSR 3.1 Frame Generation (jede GPU)',
    'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)': 'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)',
    '2× (one generated frame)': '2× (ein erzeugtes Bild)', '3× (DLSS on GeForce RTX 50)': '3× (DLSS auf GeForce RTX 50)',
    '4× (DLSS on GeForce RTX 50)': '4× (DLSS auf GeForce RTX 50)', 'No limit  ⚠': 'Keine Grenze  ⚠',
    'Precise': 'Genau', 'Debug': 'Debug',
    'Quit to Desktop in the title menu and in the System menu': '„Zum Desktop“ im Titelmenü und im Systemmenü',
    # ---- Cheats: system page ----
    'What your PC can do, rated like the hardware check on bbport.de, and game tests that measure it: the best settings '
    'for a goal, settings side by side, stability.':
        'Was dein PC kann, bewertet wie der Hardware-Check auf bbport.de, und Spieltests, die es messen: beste '
        'Einstellungen für ein Ziel, Einstellungen im Vergleich, Stabilität.',
    # ---- Help ----
    'Set everything up again, step by step.': 'Alles noch einmal Schritt für Schritt einrichten.',
    'Only with your own copy of the game': 'Nur mit deiner eigenen Kopie des Spiels',
    "Windows' title bar instead of the launcher's own": 'Windows-Titelleiste statt der eigenen des Launchers',
    "The port does not come with the game, and we can't give out game files or help anyone find them. Asking "
    'for them or sharing them breaks rule 2 of the community Discord and can get you banned.':
        'Das Spiel ist nicht im Port enthalten, und wir können keine Spieldateien weitergeben oder helfen, '
        'sie zu finden. Wer danach fragt oder sie teilt, verstößt gegen Regel 2 des Community-Discords und '
        'kann gebannt werden.',
    'The expert view is on now, to show this. The switch is at the bottom of the side bar.':
        'Die Expertenansicht ist jetzt an, um das zu zeigen. Der Schalter ist unten in der Seitenleiste.',
    'Copy a summary': 'Zusammenfassung kopieren', 'Show the log': 'Protokoll zeigen', 'Repeat the setup': 'Einrichtung wiederholen',
    'Only a black screen': 'Nur ein schwarzes Bild',
    'The game starts but shows nothing, or the picture breaks after a driver update: its shader cache no longer fits '
    'the driver.':
        'Das Spiel startet, zeigt aber nichts, oder das Bild ist nach einem Treiber-Update kaputt: Sein Shader-Cache passt '
        'nicht mehr zum Treiber.',
    'Clear the shader cache (the button below). The next start stutters for a few minutes while it is built again.':
        'Leere den Shader-Cache (Knopf unten). Der nächste Start ruckelt ein paar Minuten, während er neu gebaut wird.',
    'Still black: update the graphics driver and restart the PC.':
        'Immer noch schwarz: Aktualisiere den Grafiktreiber und starte den PC neu.',
    'Clear the shader cache': 'Shader-Cache leeren', 'Open the memory settings': 'Speicher-Einstellungen öffnen',
    'Open Windows Security': 'Windows-Sicherheit öffnen', 'Open the port folder': 'Port-Ordner öffnen',
    'Open the game folder': 'Spielordner öffnen', 'Choose the game folder': 'Spielordner wählen',
    'Copied: paste it into your message on Discord or in the forum.':
        'Kopiert: Füge es in deine Nachricht im Discord oder im Forum ein.',
    # ---- Online play ----
    'Co-op pool': 'Koop-Pool', 'Play with': 'Spielen mit', 'shadNet server': 'shadNet-Server',
    'shadNet: a shadNet server and its account. SuperMedo Server uses an account from supermedo.com.':
        'shadNet: ein shadNet-Server mit seinem Konto. Der SuperMedo Server nutzt ein Konto von supermedo.com.',
    'SuperMedo Server account': 'SuperMedo-Server-Konto', 'Username': 'Benutzername',
    'The account you made at supermedo.com.': 'Das Konto, das du auf supermedo.com angelegt hast.',
    "The Hunter's Dream account": "The-Hunter's-Dream-Konto", 'Sign up for SuperMedo Server': 'Beim SuperMedo Server registrieren',
    "The Hunter's Dream (with bbhost players)": "The Hunter's Dream (mit bbhost-Spielern)",
    'Show me on the live map': 'Mich auf der Live-Karte zeigen',
    'thehuntersdream.com/map shows where players are and which bells ring: the game sends your position more often while '
    'you play.':
        'thehuntersdream.com/map zeigt, wo Spieler sind und welche Glocken läuten: Das Spiel sendet beim Spielen öfter '
        'deine Position.',
    "The connection to The Hunter's Dream is Awaken's port of bbhost's network code; bbhost and The Hunter's Dream are "
    'by droogie.':
        "Die Verbindung zu The Hunter's Dream ist Awakens Port des Netzwerkcodes von bbhost; bbhost und The Hunter's Dream "
        'sind von droogie.',
    'Not signed in': 'Nicht angemeldet', 'Signed in as {}': 'Angemeldet als {}', 'Sign in with Discord': 'Mit Discord anmelden',
    'Starting the sign-in…': 'Anmeldung startet…', 'Sign-in failed: {}': 'Anmeldung fehlgeschlagen: {}',
    'In the page that opened ({0}), sign in with Discord and enter this code:':
        'Melde dich auf der geöffneten Seite ({0}) mit Discord an und gib diesen Code ein:',
    'Not signed in: use "Sign in with Discord" above.': 'Nicht angemeldet: Nutze oben „Mit Discord anmelden“.',
    'SuperMedo Server could not be reached. Check your internet connection and try again.':
        'Der SuperMedo Server war nicht erreichbar. Prüfe deine Internetverbindung und versuch es erneut.',
    'Game server {0}': 'Spielserver {0}', 'Co-op service {0}': 'Koop-Dienst {0}', '{0}: reachable': '{0}: erreichbar',
    '{0}: not reachable ({1})': '{0}: nicht erreichbar ({1})',
    # ---- Achievements ----
    'Trophies the game unlocks while you play; updated live, kept with your saves.':
        'Trophäen, die das Spiel beim Spielen freischaltet; live aktualisiert, bei deinen Spielständen gespeichert.',
    'All': 'Alle', 'Earned': 'Erhalten', 'Locked': 'Gesperrt', "You've earned a trophy.": 'Du hast eine Trophäe erhalten.',
    'Trophy {}': 'Trophäe {}', '{} of {} trophies earned': '{} von {} Trophäen erhalten',
    'No trophies yet. They appear here as soon as the game unlocks them.':
        'Noch keine Trophäen. Sie erscheinen hier, sobald das Spiel sie freischaltet.',
    'Every trophy is earned. Hail, good hunter.': 'Alle Trophäen erhalten. Heil dir, guter Jäger.',
    'Platinum': 'Platin', 'Gold': 'Gold', 'Silver': 'Silber', 'Bronze': 'Bronze',
    # ---- Controls ----
    'Controls': 'Steuerung',
    'Keyboard and mouse bindings, up to three per action. The controller keeps working side by side; edit its lines in '
    'input.ini directly — see docs/INPUT.md.':
        'Tastatur- und Mausbelegung, bis zu drei je Aktion. Der Controller funktioniert daneben weiter; seine Zeilen '
        'stehen direkt in input.ini — siehe docs/INPUT.md.',
    'Button prompts': 'Tastensymbole',
    "The game's button icons: your keyboard and mouse keys while you play with them, the controller's (PlayStation, "
    'Xbox or Switch) while you use it.':
        'Die Tastensymbole im Spiel: deine Tastatur- und Maustasten, solange du damit spielst, die des Controllers '
        '(PlayStation, Xbox oder Switch), solange du ihn nutzt.',
    'Follow the device in use': 'Nach dem genutzten Gerät', 'Keyboard and mouse': 'Tastatur und Maus',
    'Layout': 'Layout', 'Use the Dark Souls III layout': 'Dark-Souls-III-Layout nutzen',
    'WASD, Space to dodge, left click / Shift + left click to attack, right click / Shift + right click (or Ctrl) for '
    'the off hand, E interact, R item, F vial, Q lock on, Tab or Esc menu, and the 1:1 mouse camera. Replaces the '
    'keyboard and mouse bindings; the controller keeps its own.':
        'WASD, Leertaste zum Ausweichen, Linksklick / Umschalt + Linksklick zum Angreifen, Rechtsklick / Umschalt + '
        'Rechtsklick (oder Strg) für die Nebenhand, E Interagieren, R Gegenstand, F Fläschchen, Q Anvisieren, Tab oder '
        'Esc Menü und die 1:1-Mauskamera. Ersetzt die Tastatur- und Mausbelegung; der Controller behält seine.',
    'Click a slot, then press a key, click a mouse button or turn the wheel; Esc cancels. Right-click a slot to clear '
    'it. Movement and camera directions take keys only.':
        'Klick auf ein Feld, dann drück eine Taste, eine Maustaste oder dreh das Mausrad; Esc bricht ab. Rechtsklick '
        'leert ein Feld. Bewegungs- und Kamerarichtungen nehmen nur Tasten.',
    'Combinations: hold Shift, Ctrl or Alt while pressing the key or mouse button (up to three inputs, such as Shift + '
    'left click). A held combination wins over the bindings on its parts, so left click and Shift + left click can be '
    'two actions. ⚠ marks an input another action uses too. A key that is both an action of its own and part of a '
    'combination is pressed on the way into it: with Shift on Circle, Shift + left click starts a sprint that ends as a '
    'dodge.':
        'Kombinationen: Halte Umschalt, Strg oder Alt beim Drücken der Taste oder Maustaste (bis zu drei Eingaben, etwa '
        'Umschalt + Linksklick). Eine gehaltene Kombination hat Vorrang vor den Belegungen ihrer Teile, so können '
        'Linksklick und Umschalt + Linksklick zwei Aktionen sein. ⚠ markiert eine Eingabe, die auch eine andere Aktion '
        'nutzt. Eine Taste, die selbst eine Aktion und Teil einer Kombination ist, wird auf dem Weg dorthin mitgedrückt: '
        'Mit Umschalt auf Kreis startet Umschalt + Linksklick einen Sprint, der als Ausweichen endet.',
    "Reserved: Insert/Escape (the port's menu), F9 (route recording), and whichever key toggles mouse look or reloads "
    'input.ini (F7/F8 by default).':
        'Reserviert: Einfg/Esc (das Menü des Ports), F9 (Routenaufnahme) und die Tasten für den Mausblick und das '
        'Neuladen von input.ini (standardmäßig F7/F8).',
    'Mouse look': 'Mausblick', 'Enable mouse look': 'Mausblick einschalten',
    'F7 also toggles this in-game, instantly; F8 reloads these settings without restarting.':
        'F7 schaltet ihn auch im Spiel sofort um; F8 lädt diese Einstellungen ohne Neustart neu.',
    'Mouse controls': 'Die Maus steuert', 'Camera (right stick)': 'Kamera (rechter Stick)',
    'Movement (left stick)': 'Bewegung (linker Stick)', 'Camera': 'Kamera', 'Direct (1:1, like a PC game)': 'Direkt (1:1, wie ein PC-Spiel)',
    'Stick emulation': 'Stick-Nachbildung',
    "Direct turns the game's camera by exact angles: no stick acceleration or turn-speed cap. It falls back to the "
    'stick while locked on.':
        'Direkt dreht die Kamera um genaue Winkel: ohne Stick-Beschleunigung und Drehgrenze. Beim Anvisieren gilt wieder '
        'der Stick.',
    'Sensitivity (direct)': 'Empfindlichkeit (direkt)',
    'How far the direct camera turns per mouse movement (1.00 = 900 counts per radian).':
        'Wie weit sich die direkte Kamera je Mausbewegung dreht (1,00 = 900 Counts je Radiant).',
    'Sensitivity (stick)': 'Empfindlichkeit (Stick)',
    'How fast the camera turns for a given mouse movement in stick emulation.':
        'Wie schnell sich die Kamera bei der Stick-Nachbildung für eine Mausbewegung dreht.',
    'Minimum turn speed (stick)': 'Mindest-Drehgeschwindigkeit (Stick)',
    'A constant added on top of sensitivity; raises the floor for slow movements without affecting fast ones as much.':
        'Ein fester Zuschlag zur Empfindlichkeit; hebt langsame Bewegungen an, ohne schnelle so stark zu ändern.',
    'Vertical sensitivity': 'Vertikale Empfindlichkeit',
    'Up and down as a multiple of left and right (1.00 = the same).': 'Hoch und runter als Vielfaches von links und rechts (1,00 = gleich).',
    'Invert horizontal': 'Horizontal umkehren', 'Invert vertical': 'Vertikal umkehren',
    'Vertical sensitivity and inverting apply to both camera modes.': 'Vertikale Empfindlichkeit und Umkehren gelten für beide Kameraarten.',
    'Mouse buttons and the scroll wheel are bound in the slots above: click a slot, then click or turn the wheel. They '
    'work while the mouse is captured (mouse look on).':
        'Maustasten und Mausrad werden in den Feldern oben belegt: Feld anklicken, dann klicken oder am Rad drehen. Sie '
        'wirken, solange die Maus gefangen ist (Mausblick an).',
    'Changes apply the next time the game starts, or press F8 in-game to reload input.ini without restarting.':
        'Änderungen gelten beim nächsten Spielstart, oder drück F8 im Spiel, um input.ini ohne Neustart neu zu laden.',
    'Dark Souls III layout': 'Dark-Souls-III-Layout',
    'Replace your keyboard and mouse bindings with the Dark Souls III layout? Controller bindings stay as they are.':
        'Deine Tastatur- und Mausbelegung durch das Dark-Souls-III-Layout ersetzen? Die Controller-Belegung bleibt.',
    'Press…': 'Drück…', 'Reserved': 'Reserviert', 'Unsupported key': 'Nicht unterstützte Taste',
    'That key has no bbport name; pick another one.': 'Diese Taste hat keinen bbport-Namen; nimm eine andere.',
    'Keyboard only': 'Nur Tastatur', 'A stick direction can only be bound to a key, not a mouse button.':
        'Eine Stick-Richtung lässt sich nur auf eine Taste legen, nicht auf eine Maustaste.',
    '{} is reserved (menu, recording, or a hotkey) and cannot be bound to a game action.':
        '{} ist reserviert (Menü, Aufnahme oder ein Hotkey) und kann keiner Spielaktion zugewiesen werden.',
    'also {}': 'auch {}',
    'Movement and camera': 'Bewegung und Kamera', 'Move forward': 'Vorwärts', 'Move back': 'Rückwärts',
    'Move left': 'Links', 'Move right': 'Rechts', 'Walk (hold)': 'Gehen (halten)',
    'Dodge / dash (hold); back in menus (Circle)': 'Ausweichen / Sprinten (halten); zurück in Menüs (Kreis)',
    'Jump while dashing (L3)': 'Springen beim Sprinten (L3)', 'Lock on / reset camera (R3)': 'Anvisieren / Kamera zurücksetzen (R3)',
    'Camera up': 'Kamera hoch', 'Camera down': 'Kamera runter', 'Camera left': 'Kamera links', 'Camera right': 'Kamera rechts',
    'Combat': 'Kampf', 'Attack (R1)': 'Angriff (R1)', 'Strong attack (R2)': 'Starker Angriff (R2)',
    'Transform weapon (L1)': 'Waffe verwandeln (L1)', 'Firearm (L2)': 'Schusswaffe (L2)', 'Items and menus': 'Gegenstände und Menüs',
    'Interact; confirm in menus (Cross)': 'Interagieren; bestätigen in Menüs (Kreuz)', 'Use quick item (Square)': 'Schnellgegenstand nutzen (Quadrat)',
    'Blood vial (Triangle)': 'Blutfläschchen (Dreieck)', 'D-pad up': 'Steuerkreuz oben',
    'Switch quick item (d-pad down)': 'Schnellgegenstand wechseln (Steuerkreuz unten)',
    'Switch left hand weapon (d-pad left)': 'Linke Waffe wechseln (Steuerkreuz links)',
    'Switch right hand weapon (d-pad right)': 'Rechte Waffe wechseln (Steuerkreuz rechts)', 'Game menu (Options)': 'Spielmenü (Options)',
    'Gestures (touchpad)': 'Gesten (Touchpad)', 'Right half of the touchpad (debug menu)': 'Rechte Touchpad-Hälfte (Debug-Menü)',
    # ---- Advanced ----
    'Appearance': 'Aussehen', 'Slightly translucent window': 'Leicht durchsichtiges Fenster',
    'A hint of what lies behind the launcher: a blurred title bar (Windows 11) and the window at 98.5 %. '
    'Off: fully opaque, easiest to read.':
        'Eine Andeutung dessen, was hinter dem Launcher liegt: eine verschwommene Titelleiste (Windows 11) und das '
        'Fenster zu 98,5 % deckend. Aus: ganz deckend, am besten lesbar.',
    'Other versions…': 'Andere Versionen…', 'Other versions': 'Andere Versionen',
    'Install another version of the port, newer or older (back to {}). Saves, settings and mods are kept.':
        'Eine andere Version des Ports installieren, neuer oder älter (bis zurück zu {}). Spielstände, Einstellungen und '
        'Mods bleiben erhalten.',
    'Install the older version {} now? The launcher closes, installs it and opens again. Saves and settings are kept. '
    'To stay on it, switch off "Check for updates when the launcher opens".':
        'Die ältere Version {} jetzt installieren? Der Launcher schließt sich, installiert sie und öffnet sich wieder. '
        'Spielstände und Einstellungen bleiben. Um dabei zu bleiben, schalte „Beim Öffnen des Launchers nach Updates '
        'suchen“ aus.',
    'Loading the list of versions…': 'Lade die Liste der Versionen…', 'Install': 'Installieren', 'Close': 'Schließen',
    'No versions found.': 'Keine Versionen gefunden.', ' (latest)': ' (neueste)', ' (installed)': ' (installiert)',
    'Installed: v{}': 'Installiert: v{}', 'Version {} is already installed.': 'Version {} ist schon installiert.',
    'Could not load the list of versions: {}': 'Die Versionsliste konnte nicht geladen werden: {}',
    'Not loaded: {}: {}': 'Nicht geladen: {}: {}',
    # ---- the guide ----
    'Check the game': 'Spiel prüfen', 'Picture & controls': 'Bild & Steuerung', 'The Dream': 'Der Traum',
    'Saves & online': 'Spielstände & Online',
    'A few steps to your first hunt: the guide checks your game and your PC, sets the picture for your hardware and '
    'brings your character along. Every setting can be changed in the launcher afterwards.':
        'Ein paar Schritte bis zu deiner ersten Jagd: Der Assistent prüft dein Spiel und deinen PC, stellt das Bild für '
        'deine Hardware ein und bringt deinen Charakter mit. Jede Einstellung lässt sich danach im Launcher ändern.',
    'A few steps to your first hunt. Every setting can be changed in the launcher afterwards.':
        'Ein paar Schritte bis zu deiner ersten Jagd. Jede Einstellung lässt sich danach im Launcher ändern.',
    'Where do you come from?': 'Woher kommst du?', 'I am new to the port': 'Ich bin neu beim Port',
    "Awaken's port": 'Awakens Port', 'A PS4 (exported save)': 'Eine PS4 (exportierter Spielstand)',
    'Coming from another version, your characters are found in step IV.':
        'Kommst du von einer anderen Version, werden deine Charaktere in Schritt IV gefunden.',
    'Your game and your PC': 'Dein Spiel und dein PC',
    'Checked before the first start, so that it does not fail on something you can fix now. Nothing leaves your PC.':
        'Vor dem ersten Start geprüft, damit er nicht an etwas scheitert, das du jetzt beheben kannst. Nichts verlässt '
        'deinen PC.',
    'Your game folder: the one with eboot.bin, sce_module, sce_sys and dvdroot_ps4 directly inside.':
        'Dein Spielordner: der, in dem eboot.bin, sce_module, sce_sys und dvdroot_ps4 direkt liegen.',
    'Your game': 'Dein Spiel',
    'Next is possible once the game files are right: without that the game would not start or would crash.':
        'Weiter geht es, sobald die Spieldateien stimmen: Sonst würde das Spiel nicht starten oder abstürzen.',
    'Your graphics card is not in the list of the hardware check. Pick it or the closest model:':
        'Deine Grafikkarte steht nicht in der Liste des Hardware-Checks. Wähle sie oder das nächste Modell:',
    'Picture and controls': 'Bild und Steuerung',
    'Choose how it should feel; the settings for it come from your hardware.':
        'Wähle, wie es sich anfühlen soll; die Einstellungen dafür kommen von deiner Hardware.',
    'Fullscreen (recommended)': 'Vollbild (empfohlen)', 'What do you play with?': 'Womit spielst du?',
    'A controller (PlayStation, Xbox, Switch)': 'Mit einem Controller (PlayStation, Xbox, Switch)',
    'Keyboard and mouse: the Dark Souls III layout with the mouse camera':
        'Mit Tastatur und Maus: das Dark-Souls-III-Layout mit Mauskamera',
    'The game shows the buttons of whatever you use; the keys can be changed on the Controls page.':
        'Das Spiel zeigt die Tasten dessen, was du gerade nutzt; die Belegung lässt sich unter Steuerung ändern.',
    'Your characters and online play': 'Deine Charaktere und Online-Spiel',
    'Bring your saves along, and play with others.': 'Bring deine Spielstände mit und spiel mit anderen.',
    'Your characters from other versions': 'Deine Charaktere aus anderen Versionen', 'Play with others': 'Mit anderen spielen',
    'Play online: messages, bloodstains, summons and co-op': 'Online spielen: Nachrichten, Blutflecken, Beschwörungen und Koop',
    "With The Hunter's Dream, together with bbhost players. It needs a free sign-in with Discord, once.":
        "Mit The Hunter's Dream, zusammen mit bbhost-Spielern. Dafür einmal kostenlos mit Discord anmelden.",
    'Not signed in yet': 'Noch nicht angemeldet', 'Sign in again': 'Erneut anmelden',
    'In the page that opened, sign in with Discord and enter this code:':
        'Melde dich auf der geöffneten Seite mit Discord an und gib diesen Code ein:',
    'Other servers (shadNet, SuperMedo) and every option: the Online play page. Offline needs nothing: untick it.':
        'Andere Server (shadNet, SuperMedo) und alle Optionen: Seite „Online spielen“. Offline braucht nichts: Haken weg.',
    'On, signed in as {}': 'An, angemeldet als {}', 'On, sign-in still missing': 'An, Anmeldung fehlt noch', 'Picture': 'Bild',
    'How should the launcher look?': 'Wie soll der Launcher aussehen?',
    'Simple (recommended): the pages you need, everything technical is set for you':
        'Einfach (empfohlen): die Seiten, die du brauchst; alles Technische wird für dich eingestellt',
    'Expert: every page and every option': 'Experte: jede Seite und jede Option',
    'Switch any time at the bottom of the side bar.': 'Jederzeit umschaltbar unten in der Seitenleiste.',
    'The first start takes longer and the first minutes can stutter while the shader cache is built. That is normal.':
        'Der erste Start dauert länger, und die ersten Minuten können ruckeln, während der Shader-Cache gebaut wird. Das '
        'ist normal.',
    'Off': 'Aus', 'On': 'An',
    'Main monitor':
        'Hauptmonitor',
    'on the left':
        'links',
    'on the right':
        'rechts',
    'above':
        'oben',
    'below':
        'unten',
    'Monitor {}':
        'Monitor {}',
    'Monitor {} (not connected: the main monitor is used)':
        'Monitor {} (nicht angeschlossen: der Hauptmonitor wird genutzt)',
    'The DLC folder does not exist.':
        'Den DLC-Ordner gibt es nicht.',
    'This is the game folder. The DLC folder is the one that contains {}.':
        'Das ist der Spielordner. Der DLC-Ordner ist der, in dem {} liegt.',
    'This is the add-on itself: choose the folder that contains {}.':
        'Das ist die Erweiterung selbst: Wähle den Ordner, in dem {} liegt.',
    'The Old Hunters found ({}).':
        'The Old Hunters gefunden ({}).',
    'Rename the folder {} to {}: the game looks for that exact name.':
        'Benenne den Ordner {} in {} um: Das Spiel sucht genau diesen Namen.',
    'No add-on in this folder: The Old Hunters is a folder named {} in it.':
        'Keine Erweiterung in diesem Ordner: The Old Hunters ist darin ein Ordner namens {}.',
    'The Old Hunters found in the saves folder (addcont).':
        'The Old Hunters im Spielstandordner gefunden (addcont).',
    'Your edition (Game of the Year, CUSA03173) already includes The Old Hunters.':
        'Deine Ausgabe (Game of the Year, CUSA03173) enthält The Old Hunters schon.',
    'The Old Hunters: not found. Only needed if you own it; without it its areas stay closed.':
        'The Old Hunters: nicht gefunden. Nur nötig, wenn du es besitzt; ohne bleiben seine Gebiete verschlossen.',
    'Width × height, e.g. 2400x1080 (from 640x360 to 7680x4320).':
        'Breite × Höhe, z. B. 2400x1080 (von 640x360 bis 7680x4320).',
    'Same as the monitor':
        'Wie der Monitor',
    'Custom size…':
        'Eigene Größe…',
    'now {} × {}':
        'derzeit {} × {}',
    'Narrower than 16:9: the picture may look stretched.':
        'Schmaler als 16:9: Das Bild kann gestreckt wirken.',
    'Monitor':
        'Monitor',
    'Where the game opens, in a window and in fullscreen.':
        'Wo das Spiel aufgeht, im Fenster und im Vollbild.',
    'Window size':
        'Fenstergröße',
    'In a window: the size it opens at. You can still drag its edges; "As I left it" opens it at the size it had when you last closed the game.':
        'Im Fenster: die Größe beim Öffnen. Die Ränder kannst du weiter ziehen; „Wie beim letzten Mal“ öffnet es in der Größe, die es beim letzten Schließen des Spiels hatte.',
    'Same as the output resolution':
        'Wie die Ausgabeauflösung',
    'As I left it':
        'Wie beim letzten Mal',
    'last time {} × {}':
        'zuletzt {} × {}',
    'not played in a window yet':
        'noch nicht im Fenster gespielt',
    'Game files':
        'Spieldateien',
    'Language & sound':
        'Sprache & Ton',
    'Your game files and their add-ons. Set once, usually by the setup guide.':
        'Deine Spieldateien und ihre Erweiterungen. Einmal eingestellt, meist bei der Einrichtung.',
    "The game's text and voices, the launcher's language, how the game starts and sounds.":
        'Texte und Stimmen des Spiels, die Sprache des Launchers, wie das Spiel startet und klingt.',
    'As the game opens it': 'Wie das Spiel es öffnet',
    'Saved by hand':
        'Von Hand gesichert',
    'Before playing':
        'Vor dem Spielstart',
    'Before an import':
        'Vor einem Import',
    'Before a restore':
        'Vor dem Zurückholen',
    'Before a respec':
        'Vor einer Umverteilung',
    'From a file':
        'Aus einer Datei',
    'Earlier backup':
        'Ältere Sicherung',
    'Your characters':
        'Deine Charaktere',
    'From other versions':
        'Aus anderen Versionen',
    'No characters yet. The game makes them, or bring yours from another version (below).':
        'Noch keine Charaktere. Das Spiel legt sie an, oder du holst deine aus einer anderen Version (unten).',
    'A character without a readable name':
        'Ein Charakter ohne lesbaren Namen',
    'Slot {}':
        'Platz {}',
    'saved {}':
        'gespeichert {}',
    'Back up now':
        'Jetzt sichern',
    'Back up automatically before every start':
        'Vor jedem Spielstart automatisch sichern',
    'Only when you played since the last backup. The last {} automatic backups are kept; the ones you make yourself always.':
        'Nur wenn du seit der letzten Sicherung gespielt hast. Die letzten {} automatischen Sicherungen bleiben, deine eigenen immer.',
    'No backups yet.':
        'Noch keine Sicherungen.',
    'Show fewer':
        'Weniger zeigen',
    'Show all ({})':
        'Alle zeigen ({})',
    'Add a backup file…':
        'Sicherungsdatei hinzufügen…',
    'Restore':
        'Zurückholen',
    'A name for this backup. Enter keeps it, Esc leaves it as it was.':
        'Ein Name für diese Sicherung. Enter übernimmt ihn, Esc lässt alles, wie es war.',
    'Rename':
        'Umbenennen',
    'Copy to…':
        'Kopieren nach…',
    'Show in folder':
        'Im Ordner zeigen',
    'Delete':
        'Löschen',
    'Could not rename the backup: {}':
        'Die Sicherung ließ sich nicht umbenennen: {}',
    'Could not back up the saves: {}':
        'Die Spielstände ließen sich nicht sichern: {}',
    'There is no save to back up yet.':
        'Es gibt noch keinen Spielstand zum Sichern.',
    'Put this backup back?':
        'Diese Sicherung zurückholen?',
    'Your current saves ({}) are backed up first ("{}"), so nothing is lost.':
        'Deine aktuellen Spielstände ({}) werden vorher gesichert („{}“), es geht also nichts verloren.',
    'Could not put the backup back: {}':
        'Die Sicherung ließ sich nicht zurückholen: {}',
    "Backup put back. Your characters are in the game's Load Game menu.":
        'Sicherung zurückgeholt. Deine Charaktere findest du im Spiel unter „Spiel laden“.',
    'Choose a backup (zip)':
        'Sicherung wählen (Zip)',
    'This file is no Bloodborne save backup: {}':
        'Diese Datei ist keine Bloodborne-Spielstandsicherung: {}',
    'Copy the backup to…':
        'Sicherung kopieren nach…',
    'Could not copy the backup: {}':
        'Die Sicherung ließ sich nicht kopieren: {}',
    'Delete this backup? It goes to the Recycle Bin.':
        'Diese Sicherung löschen? Sie kommt in den Papierkorb.',
    'Could not move the backup to the Recycle Bin.':
        'Die Sicherung ließ sich nicht in den Papierkorb verschieben.',
    'The automatic backup of the saves failed: {}':
        'Die automatische Sicherung der Spielstände ist fehlgeschlagen: {}',
    'Saves backed up: {}':
        'Spielstände gesichert: {}',
    '{} online':
        '{} online',
    "The Hunter's Dream: {} hunters online right now ({} bbhost · {} shadNet)":
        "The Hunter's Dream: gerade {} Jäger online ({} bbhost · {} shadNet)",
    'A character is gone or the save is broken':
        'Ein Charakter ist weg oder der Spielstand kaputt',
    'The Load Game menu is empty, a character is missing, or the game says the save is damaged. Before every start the launcher backs up your saves, so you can go back to how they were.':
        'Das Menü „Spiel laden“ ist leer, ein Charakter fehlt, oder das Spiel meldet einen beschädigten Spielstand. Vor jedem Start sichert der Launcher deine Spielstände, du kannst also zum vorherigen Stand zurück.',
    'Close the game.':
        'Schließe das Spiel.',
    'Saves page, Backups: put back the newest backup from before the problem ("Before playing" with the date of your last good session).':
        'Seite Spielstände, Sicherungen: Hol die neueste Sicherung von vor dem Problem zurück („Vor dem Spielstart“ mit dem Datum deiner letzten guten Sitzung).',
    'Your current saves are backed up first: trying an older backup loses nothing.':
        'Deine aktuellen Spielstände werden vorher gesichert: Eine ältere Sicherung auszuprobieren kostet nichts.',
    'The character came from another version and there is no backup yet: import it again under "From other versions".':
        'Der Charakter kam aus einer anderen Version und es gibt noch keine Sicherung: Importiere ihn erneut unter „Aus anderen Versionen“.',
    'The controller does nothing':
        'Der Controller macht nichts',
    'The game reads controllers the same way the controller test on the Controls page does: what lights up there reaches the game.':
        'Das Spiel liest Controller genauso wie der Controller-Test auf der Seite Steuerung: Was dort aufleuchtet, kommt im Spiel an.',
    'Open the controller test and press the buttons.':
        'Öffne den Controller-Test und drücke die Tasten.',
    'Nothing lights up: connect the controller again (a cable is the surest), and close tools that take it over, like DS4Windows.':
        'Nichts leuchtet auf: Verbinde den Controller neu (am sichersten per Kabel) und schließe Programme, die ihn übernehmen, etwa DS4Windows.',
    "Started from Steam: in the game's Steam properties, try Steam Input on or off.":
        'Über Steam gestartet: Probiere in den Steam-Eigenschaften des Spiels Steam Input an oder aus.',
    'Open the backups':
        'Sicherungen öffnen',
    'Open the controller test':
        'Controller-Test öffnen',
    'Controller test':
        'Controller-Test',
    'Press the buttons: what lights up here reaches the game.':
        'Drück die Tasten: Was hier aufleuchtet, kommt im Spiel an.',
    'The game is running: the test waits until it is closed.':
        'Das Spiel läuft: Der Test wartet, bis es geschlossen ist.',
    "SDL3.dll is missing in the port's bin folder.":
        'SDL3.dll fehlt im bin-Ordner des Ports.',
    'The controller test cannot start: {}':
        'Der Controller-Test kann nicht starten: {}',
    'No controller found. Connect it by cable or Bluetooth.':
        'Kein Controller gefunden. Verbinde ihn per Kabel oder Bluetooth.',
    "The Hunter's Dream: co-op together with bbhost players, after a free sign-in with Discord. shadNet: a shadNet server of your choice and its account.":
        "The Hunter's Dream: Koop zusammen mit bbhost-Spielern, nach einer kostenlosen Anmeldung mit Discord. shadNet: ein shadNet-Server deiner Wahl und sein Konto.",
    'A shadNet server instead, and every option: the Online play page. Offline needs nothing: untick it.':
        'Stattdessen ein shadNet-Server und alle Optionen: Seite „Online spielen“. Offline braucht nichts: Haken weg.',
    'Fullscreen, HDR and the taste effects are on Picture & performance.':
        'Vollbild, HDR und die Geschmackseffekte sind unter Bild & Leistung.',
    'Your characters, their backups and the trophies. Empty: {}.':
        'Deine Charaktere, ihre Sicherungen und die Trophäen. Leer: {}.',
    'Cache folder':
        'Cache-Ordner',
    'Choose the cache folder':
        'Cache-Ordner wählen',
    'Shaders, logs and temporary files of this PC. Empty: in the saves folder, or on this PC when the saves folder is in a cloud folder (OneDrive, Dropbox, Google Drive), so every PC keeps its own shaders.':
        'Shader, Protokolle und temporäre Dateien dieses PCs. Leer: im Spielstandordner, oder auf diesem PC, wenn der Spielstandordner in einem Cloud-Ordner liegt (OneDrive, Dropbox, Google Drive), damit jeder PC seine eigenen Shader behält.',
    'Caches now: {}':
        'Caches jetzt: {}',
    'Your saves folder is in a cloud folder: the saves go with you, the shaders stay on this PC. Play on one PC at a time and let it finish syncing before you start on another one; in OneDrive, set the folder to "Always keep on this device".':
        'Dein Spielstandordner liegt in einem Cloud-Ordner: Die Spielstände wandern mit, die Shader bleiben auf diesem PC. Spiel immer nur auf einem PC und lass die Synchronisierung fertig werden, bevor du auf einem anderen startest; stell den Ordner in OneDrive auf „Immer auf diesem Gerät beibehalten“.',
    'Drag a mod by its grip to change the order. Lower in the list loads later and wins conflicts.':
        'Zieh einen Mod an seinem Griff, um die Reihenfolge zu ändern. Weiter unten lädt später und gewinnt bei Konflikten.',
    'Weapons':
        'Waffen',
    'Bosses':
        'Bosse',
    'Deaths':
        'Tode',
    'None beaten yet':
        'Noch keiner besiegt',
    'Earlier states in your backups ({})':
        'Frühere Stände in deinen Sicherungen ({})',
    'Hide the earlier states':
        'Frühere Stände ausblenden',
    'same as now':
        'wie jetzt',
    'In the game, {} loads the character you played last; {} on the title screen lists all of them.':
        'Im Spiel lädt „{}“ den zuletzt gespielten Charakter; „{}“ im Titelbildschirm zeigt alle.',
    'Its level, weapons and bosses cannot be read from this save.':
        'Stufe, Waffen und Bosse lassen sich aus diesem Spielstand nicht lesen.',
    'Every character in this backup goes back to this state.':
        'Alle Charaktere in dieser Sicherung gehen auf diesen Stand zurück.',
    'Put {} back to this state?':
        '{} auf diesen Stand zurückholen?',
    'Only this character changes; your other characters stay as they are. Its current state is backed up first ("{}"), so nothing is lost.':
        'Nur dieser Charakter ändert sich, deine anderen Charaktere bleiben, wie sie sind. Sein jetziger Stand wird vorher gesichert („{}“), es geht also nichts verloren.',
    'This character cannot be put back on its own ({}). Restore the whole backup under Backups instead.':
        'Dieser Charakter lässt sich nicht einzeln zurückholen ({}). Hol stattdessen unter Sicherungen die ganze Sicherung zurück.',
    '{} is back at this state.':
        '{} ist wieder auf diesem Stand.',
    'Only one character is affected: under Your characters, open its earlier states and put back just that one.':
        'Nur ein Charakter betroffen: unter „Deine Charaktere“ seine früheren Stände öffnen und nur ihn zurückholen.',
}}

for _language, _texts in UI.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(_texts)
