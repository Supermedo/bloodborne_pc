# SPDX-License-Identifier: GPL-2.0-or-later
"""Translations of the Help page (bbport_help) and the game tests (bbport_setup, the built-in
scenarios). Keyed by the English text like bbport_lang; merged into its tables on import.
Languages without an entry show the English text."""
import bbport_lang

HELP = {'de': {
    # ---- the Help page ----
    'Help': 'Hilfe',
    'The problems any installation can run into, checked for this PC, and how to fix each of them.':
        'Die Probleme, die jede Installation treffen können, für diesen PC geprüft, und wie man sie behebt.',
    'Problems and their fixes': 'Probleme und ihre Lösung', 'Still stuck?': 'Kommst du nicht weiter?',
    'No eboot.bin in the game folder: choose the folder that has eboot.bin, sce_sys and dvdroot_ps4 directly '
    'inside (Game & language).':
        'Keine eboot.bin im Spielordner: Wähle den Ordner, in dem eboot.bin, sce_sys und dvdroot_ps4 direkt liegen '
        '(Spiel & Sprache).',
    'Check again': 'Erneut prüfen', 'Start anyway?': 'Trotzdem starten?', 'Show the fix?': 'Lösung anzeigen?',
    'eboot.bin cannot be read.': 'eboot.bin kann nicht gelesen werden.',
    'Checking the game files…': 'Prüfe die Spieldateien…', 'How to fix': 'So behebst du es',
    'These mods bring their own eboot.bin: {}. Turn them off or remove that file from them.':
        'Diese Mods bringen eine eigene eboot.bin mit: {}. Schalte sie aus oder entferne die Datei daraus.',
    'The page file is off: ': 'Die Auslagerungsdatei ist aus: ', 'The port is in {}.': 'Der Port liegt in {}.',
    'Check this installation': 'Diese Installation prüfen',
    "Ask in the help forum of the community Discord. Save a report and attach it: it has the log, your settings "
    "and this PC's details (no passwords), so a helper sees the cause at once.":
        'Frag im Hilfe-Forum des Community-Discords. Speichere einen Bericht und häng ihn an: Er enthält das Log, '
        'deine Einstellungen und die Daten dieses PCs (keine Passwörter), so sieht ein Helfer die Ursache sofort.',
    'Save a report': 'Bericht speichern', 'Open the Discord': 'Discord öffnen',
    'eboot.bin has a 60 FPS patch built in: the game crashes with it (Help)':
        'In eboot.bin ist ein 60-FPS-Patch eingebaut: Das Spiel stürzt damit ab (Hilfe)',
    'eboot.bin is not the clean 1.09 one: 30 FPS, no effect options (Help)':
        'eboot.bin ist nicht die saubere 1.09: 30 FPS, keine Effekt-Optionen (Hilfe)',
    'eboot.bin is cut off: unpack the game again (Help)': 'eboot.bin ist abgeschnitten: Spiel neu entpacken (Hilfe)',
    'eboot.bin cannot be read (Help)': 'eboot.bin kann nicht gelesen werden (Hilfe)',
    'Close big programs (browsers), or let Windows manage the page file (Help shows how).':
        'Schließe große Programme (Browser) oder lass Windows die Auslagerungsdatei verwalten (Hilfe zeigt wie).',
    "eboot.bin has Lance McDonald's 60 FPS patch built in: the game crashes with it. Use the clean one.":
        'In eboot.bin ist Lance McDonalds 60-FPS-Patch eingebaut: Das Spiel stürzt damit ab. Nimm die saubere.',
    'Game version {}: it needs the 1.09 update.': 'Spielversion {}: Es braucht das Update 1.09.',
    'dvdroot_ps4 is incomplete ({} files, {} GB): this looks like the update folder alone.':
        'dvdroot_ps4 ist unvollständig ({} Dateien, {} GB): Das sieht nach dem Update-Ordner allein aus.',
    "only {} GB are free for the game's start right now, it reserves about {} GB.":
        'gerade sind nur {} GB für den Start frei, das Spiel reserviert etwa {} GB.',
    'Memory for the start: {} GB free (RAM and page file), about {} GB needed.':
        'Speicher für den Start: {} GB frei (RAM und Auslagerungsdatei), etwa {} GB nötig.',
    'Saved: {}': 'Gespeichert: {}', 'Empty files in the game folder, for example {}.':
        'Leere Dateien im Spielordner, zum Beispiel {}.',
    'Files of different versions are mixed':
        'Dateien verschiedener Versionen sind gemischt',
    "The port's program (bin\\bb-probe.exe) and its scripts come from different versions, for example after a test "
    'build was unpacked over an older version or only part of a package was copied. The game then stops at the '
    'start with a black screen (the log says "bad patch entry").':
        'Das Programm des Ports (bin\\bb-probe.exe) und seine Skripte stammen aus verschiedenen Versionen, zum '
        'Beispiel wenn eine Testversion über eine ältere Version entpackt oder nur ein Teil eines Pakets kopiert '
        'wurde. Das Spiel bleibt dann beim Start mit schwarzem Bild stehen (im Log steht „bad patch entry“).',
    'Download the whole package of the version you want to play.':
        'Lade das ganze Paket der Version herunter, die du spielen willst.',
    'Unpack all of it over this folder and replace the files, or into a new, empty folder.':
        'Entpacke alles über diesen Ordner und ersetze die Dateien, oder in einen neuen, leeren Ordner.',
    'Files of different versions in this installation: {}.':
        'Dateien verschiedener Versionen in dieser Installation: {}.',
    "The port's files all belong to version {}.":
        'Die Dateien des Ports gehören alle zu Version {}.',
    'The game folder is complete ({} files, {} GB).': 'Der Spielordner ist vollständig ({} Dateien, {} GB).',
    'The page file is off. ': 'Die Auslagerungsdatei ist aus. ', 'Could not save it: {}': 'Speichern fehlgeschlagen: {}',
    'eboot.bin is the clean 1.09 file ({} bytes).': 'eboot.bin ist die saubere 1.09-Datei ({} Bytes).',
    'eboot.bin is not the clean 1.09 file ({} bytes): the game runs at 30 FPS without the effect options.':
        'eboot.bin ist nicht die saubere 1.09-Datei ({} Bytes): Das Spiel läuft mit 30 FPS ohne die Effekt-Optionen.',
    'eboot.bin is cut off ({} bytes, the clean one has {}).':
        'eboot.bin ist abgeschnitten ({} Bytes, die saubere hat {}).',
    'eboot.bin cannot be read ({} bytes).': 'eboot.bin kann nicht gelesen werden ({} Bytes).',
    'The check stopped: {}': 'Die Prüfung brach ab: {}',
    'Windows has only {} GB free for programs right now; the game reserves about {} GB at its start and would '
    'close at once (code 3).':
        'Windows hat gerade nur {} GB für Programme frei; das Spiel reserviert beim Start etwa {} GB und würde '
        'sofort schließen (Code 3).',
    'The game ended with an error (code {}). Its log names the cause:':
        'Das Spiel endete mit einem Fehler (Code {}). Sein Log nennt die Ursache:',
    # the problems
    'eboot.bin is not the clean 1.09 one': 'eboot.bin ist nicht die saubere 1.09',
    "Re-uploaded 1.09 updates often have a 60 FPS patch or other changes built into eboot.bin. The port brings its "
    "own 60 FPS and patches only the clean file: with a changed one the game runs at 30 FPS, the effect "
    "options do nothing, and some patches (Lance McDonald's 60 FPS) crash the gestures menu or the cutscene after "
    "Rom.":
        'Neu hochgeladene 1.09-Updates haben oft einen 60-FPS-Patch oder andere Änderungen in der eboot.bin. Der '
        'Port bringt seine eigenen 60 FPS mit und patcht nur die saubere Datei: Mit einer veränderten läuft das Spiel '
        'mit 30 FPS, die Effekt-Optionen wirken nicht, und manche Patches (Lance McDonalds 60 FPS) '
        'lassen das Gestenmenü oder die Zwischensequenz nach Rom abstürzen.',
    "Use the eboot.bin of an unmodified 1.09 update, in a new, empty folder. Don't use a loose eboot.bin "
    'someone shared.':
        'Nimm die eboot.bin eines unveränderten Updates 1.09, in einem neuen, leeren Ordner. Keine einzelne '
        'eboot.bin, die jemand geteilt hat.',
    'Copy eboot.bin from it into your game folder (Game & language > Game folder) and replace the old one. Make sure '
    'it is that folder, not another copy of the game.':
        'Kopiere die eboot.bin daraus in deinen Spielordner (Spiel & Sprache > Spielordner) und ersetze die alte. '
        'Achte darauf, dass es genau dieser Ordner ist und keine andere Kopie des Spiels.',
    'Right-click eboot.bin > Properties: the clean file is exactly 93,825,704 bytes.':
        'Rechtsklick auf eboot.bin > Eigenschaften: Die saubere Datei hat genau 93.825.704 Bytes.',
    'Mods must not bring their own eboot.bin: the check at the top of this page names any that do.':
        'Mods dürfen keine eigene eboot.bin mitbringen: Die Prüfung oben nennt solche Mods.',
    'eboot.bin is cut off': 'eboot.bin ist abgeschnitten',
    'It broke while unpacking (the log says "prepare failed: out of bounds").':
        'Sie ist beim Entpacken kaputtgegangen (im Log steht „prepare failed: out of bounds“).',
    'Put the base game and the 1.09 update into two new, empty folders again.':
        'Lege das Grundspiel und das Update 1.09 erneut in zwei neue, leere Ordner.',
    'Check the size: eboot.bin of the update is exactly 93,825,704 bytes (89.4 MB).':
        'Prüfe die Größe: Die eboot.bin des Updates hat genau 93.825.704 Bytes (89,4 MB).',
    'Copy everything from the update folder into the base game folder and replace the files.':
        'Kopiere alles aus dem Update-Ordner in den Basisspiel-Ordner und ersetze die Dateien.',
    'eboot.bin cannot be read': 'eboot.bin kann nicht gelesen werden',
    'It is not the original file (a PS4 SELF file): another tool has converted it, or it is not from '
    'Bloodborne.':
        'Es ist nicht die Originaldatei (eine PS4-SELF-Datei): Ein anderes Werkzeug hat sie umgewandelt, '
        'oder sie ist nicht von Bloodborne.',
    'Use the eboot.bin of an unmodified 1.09 update (exactly 93,825,704 bytes).':
        'Nimm die eboot.bin eines unveränderten Updates 1.09 (genau 93.825.704 Bytes).',
    'The game is not version 1.09': 'Das Spiel ist nicht Version 1.09',
    'The community patches (60 FPS and more, the effect options) exist for 1.09 only; other versions run at 30 FPS.':
        'Die Community-Patches (60 FPS und mehr, die Effekt-Optionen) gibt es nur für 1.09; andere Versionen laufen '
        'mit 30 FPS.',
    'Put the 1.09 update into a new, empty folder again.': 'Lege das Update 1.09 erneut in einen neuen, '
                                                            'leeren Ordner.',
    'Open it, press Ctrl + A, then Ctrl + C.': 'Öffne ihn, drücke Strg + A, dann Strg + C.',
    'Open the base game folder (the one with dvdroot_ps4), press Ctrl + V and choose Replace the files in the '
    'destination.':
        'Öffne den Basisspiel-Ordner (den mit dvdroot_ps4), drücke Strg + V und wähle „Dateien im Ziel ersetzen“.',
    'Only the update folder was chosen': 'Nur der Update-Ordner wurde gewählt',
    'The folder has eboot.bin but not the whole game (the log says gxshader.shaderbnd.dcx not found).':
        'Der Ordner hat eine eboot.bin, aber nicht das ganze Spiel (im Log steht gxshader.shaderbnd.dcx not found).',
    'Copy everything from the update folder into the base game folder (replace the files).':
        'Kopiere alles aus dem Update-Ordner in den Basisspiel-Ordner (Dateien ersetzen).',
    'Choose the base game folder in Game & language. Its dvdroot_ps4 has about 29 GB in about 28,800 files.':
        'Wähle unter Spiel & Sprache den Basisspiel-Ordner. Sein dvdroot_ps4 hat etwa 29 GB in etwa 28.800 Dateien.',
    'Files of the game are empty': 'Dateien des Spiels sind leer',
    'Unpacking broke: some files have 0 bytes.': 'Das Entpacken ist schiefgegangen: Manche Dateien haben 0 Bytes.',
    'Put the base game and the 1.09 update into new, empty folders again, then copy the update into the base '
    'game again.':
        'Lege das Grundspiel und das Update 1.09 erneut in neue, leere Ordner und kopiere das Update dann '
        'wieder ins Grundspiel.',
    'Windows has too little memory for the start': 'Windows hat zu wenig Speicher für den Start',
    'The game reserves about 6 GB at its start (10 GB at 1440p and 4K) in one block, from the RAM and the page file '
    'together. When Windows refuses, the game closes at once with code 3 (the log says "cannot create … MiB of guest '
    'memory").':
        'Das Spiel reserviert beim Start etwa 6 GB (10 GB in 1440p und 4K) am Stück, aus RAM und Auslagerungsdatei '
        'zusammen. Lehnt Windows ab, schließt das Spiel sofort mit Code 3 (im Log steht „cannot create … MiB of '
        'guest memory“).',
    'Press Win + R, type sysdm.cpl, press Enter.': 'Drücke Win + R, tippe sysdm.cpl, drücke Enter.',
    'Advanced > Performance: Settings… > Advanced > Virtual memory: Change…':
        'Erweitert > Leistung: Einstellungen… > Erweitert > Virtueller Arbeitsspeicher: Ändern…',
    'Tick Automatically manage paging file size for all drives. Still crashing: untick it, select a drive with free '
    'space, Custom size: initial 16384, maximum 32768, then Set.':
        'Hake „Auslagerungsdateigröße für alle Laufwerke automatisch verwalten“ an. Stürzt es weiter ab: Haken weg, '
        'ein Laufwerk mit freiem Platz wählen, Benutzerdefinierte Größe: Anfangsgröße 16384, Maximale Größe 32768, '
        'dann Festlegen.',
    'OK, then restart the PC: the change works only after a restart.':
        'OK, dann den PC neu starten: Die Änderung wirkt erst nach einem Neustart.',
    'Close browsers and other big programs before you press PLAY.':
        'Schließe Browser und andere große Programme, bevor du SPIELEN drückst.',
    "An antivirus blocks the port's files": 'Ein Virenscanner blockiert die Dateien des Ports',
    'The launcher rebuilds a few files at every start; an antivirus that scans them at that moment blocks them (the '
    'log says PermissionError [WinError 32] or WinError 5).':
        'Der Launcher baut bei jedem Start ein paar Dateien neu; ein Virenscanner, der sie in dem Moment prüft, '
        'blockiert sie (im Log steht PermissionError [WinError 32] oder WinError 5).',
    'Start menu > Windows Security > Virus & threat protection > Manage settings.':
        'Startmenü > Windows-Sicherheit > Viren- & Bedrohungsschutz > Einstellungen verwalten.',
    'Exclusions > Add or remove exclusions > Add an exclusion > Folder: the port folder (the one with Bloodborne.exe).':
        'Ausschlüsse > Ausschlüsse hinzufügen oder entfernen > Ausschluss hinzufügen > Ordner: der Port-Ordner (der '
        'mit Bloodborne.exe).',
    'Another antivirus (Avast, Norton, …): add the port folder to its exceptions there.':
        'Anderer Virenscanner (Avast, Norton, …): Trag den Port-Ordner dort als Ausnahme ein.',
    'Still there: move the port to a plain folder like C:\\Games\\BBPort.':
        'Immer noch: Verschiebe den Port in einen einfachen Ordner wie C:\\Games\\BBPort.',
    'The port is in a synced or protected folder': 'Der Port liegt in einem synchronisierten oder geschützten Ordner',
    'OneDrive (Desktop and Documents often are synced) and Program Files lock or move files while the port writes '
    'them.':
        'OneDrive (Desktop und Dokumente werden oft synchronisiert) und „Programme“ sperren oder verschieben Dateien, '
        'während der Port sie schreibt.',
    'Move the whole port folder to a plain folder like C:\\Games\\BBPort and start Bloodborne.exe from there.':
        'Verschiebe den ganzen Port-Ordner in einen einfachen Ordner wie C:\\Games\\BBPort und starte Bloodborne.exe '
        'von dort.',
    "The graphics card's memory runs full": 'Der Grafikspeicher läuft voll',
    'Cards with 6 GB or less, or the log says ErrorOutOfDeviceMemory or "memory pressure": textures no longer fit, '
    'the game stutters, shows broken faces or crashes.':
        'Karten mit 6 GB oder weniger, oder im Log steht ErrorOutOfDeviceMemory oder „memory pressure“: Texturen '
        'passen nicht mehr, das Spiel ruckelt, zeigt kaputte Gesichter oder stürzt ab.',
    'Graphics: preset Balanced or Performance (DLSS on GeForce RTX cards).':
        'Grafik: Qualitätsstufe Balanced oder Performance (DLSS auf GeForce-RTX-Karten).',
    'Model detail (LOD) "As in the game", object motion vectors off.':
        'Modelldetail (LOD) „Wie im Spiel“, Objekt-Bewegungsvektoren aus.',
    'Close your browser and other programs while playing.': 'Schließe beim Spielen den Browser und andere Programme.',
    'The graphics driver stopped (device lost)': 'Der Grafiktreiber ist ausgestiegen (device lost)',
    'The game needs Vulkan 1.3 and a current driver; an old or crashed driver ends the game with "device lost".':
        'Das Spiel braucht Vulkan 1.3 und einen aktuellen Treiber; ein alter oder abgestürzter Treiber beendet das '
        'Spiel mit „device lost“.',
    'Update the driver: NVIDIA App, AMD Software (Adrenalin) or Intel Driver & Support Assistant.':
        'Treiber aktualisieren: NVIDIA App, AMD Software (Adrenalin) oder Intel Driver & Support Assistant.',
    'Restart the PC after installing it.': 'Starte den PC nach der Installation neu.',
    'Overclocked graphics card or memory: try the standard clocks.':
        'Übertaktete Grafikkarte oder Speicher: Probier die Standardtakte.',
    'Stutter in the first minutes': 'Ruckler in den ersten Minuten',
    'Normal: the port builds its shader cache the first time it sees each effect. It is kept for the next starts.':
        'Normal: Der Port baut seinen Shader-Cache, wenn er einen Effekt zum ersten Mal sieht. Er bleibt für die '
        'nächsten Starts erhalten.',
    'Play on: it gets smooth once an area has been seen. After a driver update the cache is built again.':
        'Einfach weiterspielen: Es wird flüssig, sobald ein Gebiet einmal gesehen wurde. Nach einem Treiber-Update '
        'wird der Cache neu gebaut.',
    '"Windows protected your PC"': '„Der Computer wurde durch Windows geschützt“',
    'Windows shows this for programs it does not know yet.': 'Das zeigt Windows bei Programmen, die es noch nicht kennt.',
    'Click More info, then Run anyway.': 'Klicke auf „Weitere Informationen“, dann auf „Trotzdem ausführen“.',
    # ---- the game tests ----
    'Game tests': 'Spieltests', 'Test': 'Test', 'Scenario': 'Szenario', 'Goal': 'Ziel', 'Duration': 'Dauer',
    'Values': 'Werte', 'Each': 'Jeder Wert', 'Save': 'Spielstand', 'now': 'aktuell', 'On': 'An', 'Off': 'Aus',
    'The launcher starts the game from your last save, the same way every time, and measures everything while it '
    'runs: frame rate, frame times, stutter, memory, processor and graphics card load. It plays on a copy of your '
    'save in a folder of its own: your saves and settings stay as they are. The report has every chart, the pictures '
    'and the logs, to keep or to share.':
        'Der Launcher startet das Spiel von deinem letzten Spielstand, jedes Mal gleich, und misst alles, während es '
        'läuft: Bildrate, Frametimes, Ruckler, Speicher, Prozessor- und Grafikkartenlast. Er spielt mit einer Kopie '
        'deines Spielstands in einem eigenen Ordner: Deine Spielstände und Einstellungen bleiben unberührt. Der '
        'Bericht enthält alle Graphen, die Bilder und die Logs, zum Aufheben oder Teilen.',
    'Best settings for a frame rate goal': 'Beste Einstellungen für ein FPS-Ziel',
    'Compare one setting: pictures and frame rate': 'Eine Einstellung vergleichen: Bilder und Bildrate',
    'Stability test (for beta builds)': 'Stabilitätstest (für Beta-Versionen)',
    '60 FPS: the best picture that keeps 60': '60 FPS: das beste Bild, das 60 hält', '90 FPS': '90 FPS',
    '120 FPS: as smooth as the game allows': '120 FPS: so flüssig, wie das Spiel erlaubt',
    '10 minutes': '10 Minuten', '30 minutes': '30 Minuten', '1 hour': '1 Stunde', '2 hours': '2 Stunden',
    'once (about 1½ min per value)': 'einmal (etwa 1½ Min. pro Wert)',
    'twice, interleaved (more exact)': 'zweimal, abwechselnd (genauer)',
    "Hunter's view": 'Blick des Jägers',
    "From your last save: the camera turns once around the hunter and back (24 s). Quick and the same every time; "
    "for the frame rate and for comparing settings.":
        'Von deinem letzten Spielstand: Die Kamera dreht sich einmal um den Jäger und zurück (24 s). Schnell und jedes '
        'Mal gleich; für die Bildrate und zum Vergleichen von Einstellungen.',
    'Boss fight (your save at a boss)': 'Bosskampf (dein Spielstand vor einem Boss)',
    "Starts from a save in front of a boss fog wall (for example from a boss save mod): walks into the fog, locks on "
    "and fights for two minutes while the hunter cannot die. Heavy on effects and the processor, never quite the same "
    "twice: for stability and for a worst case, not for exact comparisons.":
        'Startet von einem Spielstand vor einer Boss-Nebelwand (zum Beispiel aus einer Boss-Save-Mod): Läuft in den '
        'Nebel, visiert an und kämpft zwei Minuten, ohne dass der Jäger sterben kann. Fordernd für Effekte und '
        'Prozessor, nie ganz gleich: für Stabilität und den schlimmsten Fall, nicht für genaue Vergleiche.',
    'Boss: Cleric Beast': 'Boss: Kleriker-Bestie',
    'Run: Central Yharnam sewers': 'Lauf: Kanalisation von Zentral-Yharnam', 'Run: Old Yharnam': 'Lauf: Alt-Yharnam',
    "Runs the same two-minute route through this area and back every time, from the save of the mod “All Boss Save "
    "Files Ready to Fight” (Nexus Mods). The hunter cannot die and enemies leave him alone: for frame rate, loading "
    "and stutter while moving, and for comparing settings.":
        'Läuft jedes Mal dieselbe Zwei-Minuten-Strecke durch dieses Gebiet und zurück, vom Spielstand der Mod „All Boss '
        'Save Files Ready to Fight“ (Nexus Mods). Der Jäger kann nicht sterben, Gegner lassen ihn in Ruhe: für '
        'Bildrate, Nachladen und Ruckler in Bewegung und zum Vergleichen von Einstellungen.',
    'Route': 'Strecke',
    'Not recorded yet: someone runs it once in the game (about two minutes, and back), then every test replays exactly '
    'that.':
        'Noch nicht aufgenommen: Jemand läuft sie einmal im Spiel (etwa zwei Minuten, und zurück), danach spielt jeder '
        'Test genau das ab.',
    'Record the route…': 'Strecke aufnehmen…',
    'The route of this test is not recorded yet: record it first.':
        'Die Strecke dieses Tests ist noch nicht aufgenommen: nimm sie zuerst auf.',
    "The game starts from this test's save; the hunter cannot die and enemies leave him alone.\n\n"
    "1. Load the save (Continue) and wait until you can move.\n"
    "2. Press F9 and run the route: about two minutes, and back to where you started (so the test can repeat it). "
    "Move the camera calmly, the way a player would.\n"
    "3. Press F9 again, then quit the game.\n\n"
    "Every test of this scenario then replays exactly that.":
        'Das Spiel startet vom Spielstand dieses Tests; der Jäger kann nicht sterben, Gegner lassen ihn in Ruhe.\n\n'
        '1. Lade den Spielstand (Fortsetzen) und warte, bis du dich bewegen kannst.\n'
        '2. Drück F9 und lauf die Strecke: etwa zwei Minuten, und zurück zum Startpunkt (damit der Test sie '
        'wiederholen kann). Bewege die Kamera ruhig, so wie ein Spieler.\n'
        '3. Drück wieder F9 und beende das Spiel.\n\n'
        'Jeder Test dieses Szenarios spielt danach genau das ab.',
    'The game is open: load the save, press F9, run the route and back, press F9 again, quit the game.':
        'Das Spiel ist offen: Spielstand laden, F9 drücken, die Strecke hin und zurück laufen, wieder F9, Spiel beenden.',
    'Recorded: every test of this scenario now replays your route.':
        'Aufgenommen: Jeder Test dieses Szenarios spielt jetzt deine Strecke ab.',
    'Nothing was recorded (press F9 to start and F9 to stop).':
        'Es wurde nichts aufgenommen (F9 startet, F9 stoppt).',
    'A key, the mouse or a controller used during the test ends it.':
        'Eine Taste, die Maus oder ein Controller während des Tests beendet ihn.',
    'stopped: your input': 'abgebrochen: deine Eingabe',
    'The test stopped: a key, the mouse or a controller was used. Start it again and leave the PC alone until it is '
    'done.':
        'Der Test wurde abgebrochen: Eine Taste, die Maus oder ein Controller wurde benutzt. Starte ihn neu und lass '
        'den PC in Ruhe, bis er fertig ist.',
    "Starts in front of this boss with the save from the mod “All Boss Save Files Ready to Fight” (Nexus Mods), walks "
    "in and keeps the boss busy for two minutes: dodging, strafing, now and then an attack. The hunter cannot die; the "
    "boss fights as usual. For stability and a worst case, not for exact comparisons.":
        'Startet vor diesem Boss mit dem Spielstand aus der Mod „All Boss Save Files Ready to Fight“ (Nexus Mods), '
        'läuft hinein und beschäftigt den Boss zwei Minuten lang: ausweichen, seitwärts laufen, ab und zu ein Angriff. '
        'Der Jäger kann nicht sterben, der Boss kämpft normal. Für Stabilität und den schlimmsten Fall, nicht für '
        'genaue Vergleiche.',
    'Needs the boss saves of the mod “{}” (Nexus Mods, free with an account): download “{}”, unpack it, then choose '
    'its folder.':
        'Braucht die Boss-Spielstände der Mod „{}“ (Nexus Mods, kostenlos mit Konto): „{}“ herunterladen, entpacken, '
        'dann den Ordner wählen.',
    'Download page': 'Download-Seite',
    'The save for this test is not in that folder. Choose the unpacked “{}” folder of the mod.':
        'Der Spielstand für diesen Test ist nicht in diesem Ordner. Wähle den entpackten Ordner „{}“ der Mod.',
    'Added. Choose it under Scenario (the list is updated when the launcher starts next).':
        'Hinzugefügt. Wähle es unter Szenario (die Liste wird beim nächsten Launcher-Start aktualisiert).',
    'the settings for your PC': 'die Einstellungen für deinen PC', 'up to 7 runs': 'bis zu 7 Durchläufe',
    'Start the test': 'Test starten', 'Cancel': 'Abbrechen', 'The save to start from': 'Der Spielstand zum Starten',
    'There is no save of this port, shadPS4 or bbhost in that folder (SPRJ0005 with userdata0000 …).':
        'In diesem Ordner ist kein Spielstand dieses Ports, von shadPS4 oder bbhost (SPRJ0005 mit userdata0000 …).',
    'A test scenario (scenario.json, a .json or a .zip)': 'Ein Testszenario (scenario.json, eine .json oder eine .zip)',
    'Close the game first.': 'Schließe zuerst das Spiel.',
    'The test needs your game folder and a save to start from: play until the game has saved once.':
        'Der Test braucht deinen Spielordner und einen Spielstand zum Starten: Spiel, bis das Spiel einmal '
        'gespeichert hat.',
    'Choose the save this scenario starts from first.': 'Wähle zuerst den Spielstand, von dem dieses Szenario startet.',
    'This scenario has no loop for a stability test.': 'Dieses Szenario hat keine Schleife für einen Stabilitätstest.',
    'Game test running: the game opens and closes by itself.': 'Spieltest läuft: Das Spiel öffnet und schließt sich selbst.',
    "Your PC's recommendation": 'Die Empfehlung für deinen PC', 'Stability run': 'Stabilitätslauf',
    'Lowest settings (what your PC can reach at all)': 'Niedrigste Einstellungen (was dein PC überhaupt schafft)',
    'The test did not run.': 'Der Test ist nicht gelaufen.',
    'Best for your goal: {} with {:.0f} FPS.': 'Am besten für dein Ziel: {} mit {:.0f} FPS.',
    'No setting kept the goal; this one came closest.': 'Keine Einstellung hielt das Ziel; diese kam am nächsten.',
    '(This scenario is not made for this kind of test.)': '(Dieses Szenario ist nicht für diese Testart gedacht.)',
    'Stopping: the game closes…': 'Wird beendet: Das Spiel schließt…', 'starting…': 'startet…',
    'No run finished; the report has the logs.': 'Kein Durchlauf wurde fertig; der Bericht enthält die Logs.',
    'Add a test…': 'Test hinzufügen…', 'Earlier tests': 'Frühere Tests', 'Test scenarios': 'Testszenarien',
    'Choose at least two values to compare.': 'Wähle mindestens zwei Werte zum Vergleichen.',
    '{} runs': '{} Durchläufe', '{} minutes': '{} Minuten',
    'Preparing ({}). The game opens in fullscreen; leave the PC alone until it is done.':
        'Vorbereitung ({}). Das Spiel öffnet sich im Vollbild; lass den PC in Ruhe, bis es fertig ist.',
    'measuring…': 'misst…', 'Save as zip': 'Als ZIP speichern', 'Open the folder': 'Ordner öffnen',
    'Your PC reaches about {:.0f} FPS at most here: above that the processor or the game itself sets the limit, not '
    'the graphics settings.':
        'Dein PC schafft hier höchstens etwa {:.0f} FPS: Darüber setzt der Prozessor oder das Spiel selbst die Grenze, '
        'nicht die Grafikeinstellungen.',
    'Apply': 'Übernehmen', 'This is not a test scenario: {}': 'Das ist kein Testszenario: {}',
    'Run {}: the game is starting.': 'Durchlauf {}: Das Spiel startet.', 'Open the report': 'Bericht öffnen',
    'No crash and no freeze in {:.0f} minutes ({:.0f} FPS on average).':
        'Kein Absturz und kein Hänger in {:.0f} Minuten ({:.0f} FPS im Schnitt).',
    'The game {} after {:.0f} minutes: the report has the log lines and charts up to that moment.':
        'Das Spiel ist nach {1:.0f} Minuten {0}: Der Bericht hat die Log-Zeilen und Graphen bis zu diesem Moment.',
    'Done. The report shows the pictures side by side with the frame rates; the fastest here: {} ({:.0f} FPS).':
        'Fertig. Der Bericht zeigt die Bilder nebeneinander mit den Bildraten; am schnellsten hier: {} ({:.0f} FPS).',
    '(downsampled)': '(heruntergerechnet)', 'Choose…': 'Wählen…',
    'choose the save to start from (a folder with SPRJ0005 or its files)':
        'wähle den Spielstand zum Starten (einen Ordner mit SPRJ0005 oder seinen Dateien)',
    'With your own settings. A crash, a freeze or memory that keeps growing ends up in the report with the log lines '
    'around it.':
        'Mit deinen eigenen Einstellungen. Ein Absturz, ein Hänger oder immer weiter wachsender Speicher landet mit '
        'den Log-Zeilen drumherum im Bericht.',
    'round {}, {} min left': 'Runde {}, noch {} Min.',
    "But the game's memory grew by {:.1f} GB: see the report.":
        'Aber der Speicher des Spiels wuchs um {:.1f} GB: siehe Bericht.',
    'crashed': 'abgestürzt', 'froze': 'eingefroren', 'failed': 'fehlgeschlagen', 'did not start': 'nicht gestartet',
    'cancelled': 'abgebrochen', 'Applied (Undo on this page).': 'Übernommen (Rückgängig auf dieser Seite).',
    'The test stopped: {}': 'Der Test brach ab: {}',
    'A game test is running. Stop it and close the launcher?': 'Ein Spieltest läuft. Abbrechen und den Launcher schließen?',
}}

for _language, _texts in HELP.items():
    bbport_lang.EXTRA_TRANSLATIONS.setdefault(_language, {}).update(_texts)
