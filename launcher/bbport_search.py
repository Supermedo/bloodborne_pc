# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's search: a field at the top of the side bar (Ctrl+F) that finds settings, pages
and Help topics by the words players use ("ruckelt", "black screen", "controller"), not only by
the settings' names. Enter or a click opens the page, scrolls to the setting and marks it for a
moment; settings of the expert view are found in the simple view too (and say so).

Every row, switch and folder field registers itself while the pages are built (index_entry);
the pages, the Help topics and a few cards are added in search_ready(). Words in WORDS add the
players' words to a setting (by its key), a page or a Help topic, in German and English. The side
bar's expert view switch and website button are found too and marked where they are; a row shown
only for another choice (index_only) is found with a note and leads to that choice.

Players type questions ("where is clear shader cache", "how do i update"): the words around what
they mean are left out (STOP_WORDS), and when no entry has every word, the entries with the most of
them are shown instead of nothing.
"""
import re
import unicodedata

import bbport_theme as theme

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts) if L else texts[0]


# The players' words for a setting key, a page ('page:…') or a Help topic ('topic:…').
WORDS = {
    'fullscreen': 'vollbild fenster fullscreen window borderless randlos',
    'exclusive_fullscreen': 'exklusiv exclusive display mode bildwiederholrate refresh',
    'hdr': 'hdr helligkeit brightness',
    'vram_warning': 'vram videospeicher grafikspeicher warnung warning popup banner meldung hinweis',
    'fps_mode': 'fps bildrate framerate frame rate 30 60 120 unbegrenzt unlock uncap uncapped flussig ruckelt lag',
    'frame_cap': 'begrenzen begrenzung maximal max limiter fps grenze limit cap 30 60 120 144 165 240',
    'frames_ahead': 'latenz input lag ruckelt stutter stuttering stutters lagging laggy',
    'present_mode': 'vsync tearing mailbox fifo',
    'output_res': 'auflosung resolution 4k 1440p 1080p 2160p 720p scharf unscharf pixelig eigene custom '
                  'benutzerdefiniert monitor',
    'game_monitor': 'second multi multiple monitor bildschirm zweiter display screen dual mehrere',
    'window_size': 'fenstergrosse fenster grosse window size fenstermodus windowed',
    'upscaler': 'upscaler fsr dlss taa hochskalierung kantenglattung scharf unscharf',
    'preset': 'preset stufe qualitat quality balanced performance native aa scharfer schoner',
    'sharpen': 'sharpen sharpening nachscharfen scharf scharfe unscharf blurry verwaschen rcas', 'sharpness': 'sharpness sharpening starke scharf scharfe unscharf blurry rcas',
    'object_motion': 'ghosting schlieren nachziehen bewegungsvektoren',
    'model_lod': 'lod detail modelldetail details pop-in plopp modelle sichtweite',
    'show_fps': 'fps zahler counter anzeige overlay',
    'button:news': 'news changelog patch notes release notes neuigkeiten anderungen whats new',
    'show_latency': 'latency latenz input lag eingabeverzogerung reflex anti-lag delay verzogerung',
    'show_frame_times': 'frametime frame time ms stutter ruckler',
    'frame_gen': 'framegen frame generation zwischenbilder dlss fsr fg',
    'hud_position': 'hud ultrawide 21:9 32:9 interface',
    'sharp_ui': 'sharp ui interface hud menu text blurry crisp 1440p 4k scaleform',
    'live_resolution': 'auflosung neustart restart',
    'effect_motion_blur': 'unscharfe motion blur bewegungsunscharfe',
    'effect_dof': 'tiefenscharfe depth of field unscharf hintergrund',
    'effect_chromatic_aberration': 'chromatische aberration farbsaum farbsaum chromatic aberration farbrander',
    'effect_ssao': 'ssao umgebungsverdeckung schatten ambient occlusion',
    'effect_dynamic_shadows': 'schatten shadows licht',
    'effect_ssr': 'spiegelungen reflections ssr',
    'effect_game_aa': 'kantenglattung anti aliasing aa treppen',
    'language': 'textsprache sprache language deutsch englisch untertitel text subtitles',
    'voice_language': 'japanisch japanese englisch english deutsch voices sprachausgabe stimmen voice sprache synchro audio',
    'player_name': 'player psn-name name psn spielername',
    'game_dir': 'game dump spielordner eboot cusa03173 ordner pfad folder spiel',
    'dlc_dir': 'dlc old hunters erweiterung',
    'cache_dir': 'cache shader shaders logs temp ordner folder onedrive cloud lokal local zwei pcs two pcs',
    'characters': 'charakter character level stufe waffen weapons bosse bosses besiegt getötet beaten killed '
                  'attribute stats vitalität spielzeit play time tode deaths früher earlier stand',
    'user_dir': 'spielstandordner folder speicherort spielstande saves save speicherstand ordner location',
    'skip_intro': 'intro logos video uberspringen skip',
    'mute_unfocused': 'ton stumm sound mute hintergrund lautstarke volume audio',
    'cheat_no_death': 'unsterblich unverwundbar nie sterben god mode godmode immortal invincible',
    'button:shortcut': 'desktop verknupfung verknuepfung shortcut icon symbol',
    'button:steam': 'steam bibliothek library non-steam deck',
    'button:port_folder': 'ordner port folder installation explorer',
    'button:updates': 'update updates aktualisieren neue neueste version launcher latest newest',
    'button:report': 'bericht report support zip discord fehler bug bugs melden',
    'button:game_files': 'spieldateien prufen pruefen verify integrity game files beschadigt kaputt damaged broken '
                         'dump checksum',
    'button:discord': 'discord community gemeinschaft server einladung invite chat',
    'button:forum': 'forum support frage question fragen ask thema topic beitrag post bbport.de',
    'button:benchmark': 'benchmark test messen route kathedrale cathedral fps tester',
    'button:shader_cache': 'shader cache leeren loschen clear schwarz schwarzer bildschirm black screen '
                           'grafikfehler treiber driver',
    'button:versions': 'version versionen andere altere alte fruhere older old previous rollback downgrade upgrade '
                       'zuruck installieren',
    'button:respec': 'respec umverteilen neu verteilen attribute attributpunkte punkte stats skillung',
    'button:fsr4': 'fsr4 fsr 4 herunterladen download dateien fehlen assets',
    'button:fsr4vk': 'fsr4vk fsr 4.1 fsr411 herunterladen download reparieren vulkan',
    'online': 'online koop coop co-op multiplayer seamless freunde friends friend glocke bell beschworen summon '
              'summoning invasion passwort password',
    'online_pool': 'server online shadnet supermedo hunters dream thd bbhost spielen mit play with koop coop co-op '
                   'seamless freunde friends friend summon bell passwort password',
    # "install" on the page only: alone it is the port's install guide (the side bar's website button)
    'mods_enabled': 'mods mod remaster texturen add hinzufugen',
    'translucent_window': 'transparent durchsichtig transparenz glas acrylic opacity deckkraft see through '
                          'unreadable unlesbar',
    'ui_language': 'sprache language launcher',
    'draw_pipe': 'absturz crash instabil pipeline',
    'on_play': 'schliessen beenden minimieren close minimize',
    'minimize_to_tray': 'tray taskleiste infobereich symbol notification area',
    'reopen_after_game': 'wieder oeffnen reopen back',
    'discord_status': 'discord rich presence status profil profile activity aktivitaet',
    'menu_key': 'overlay menu insert hotkey taste einstellungen ingame in-game key rebind',
    'menu_pad': 'overlay menu l3 r3 select start controller gamepad combo',
    'windows_title_bar': 'titelleiste title bar caption rahmen fensterrahmen windows',
    'goal': 'ziel goal flussig ausgewogen leise smooth balanced quiet laptop akku',
    'play_with': 'spiele mit play with eingabe input gerat device controller gamepad tastatur keyboard maus mouse '
                 'mauskamera kb m+k wasd',
    'auto_backup': 'sicherung sichern backup automatisch spielstand',
    # The backups' entries (and auto_backup above) have "save" and "spielstand" without their plurals: "where
    # are my saves" and "wo sind meine spielstände" are the saves folder and the Saves page. Restoring has its
    # own entry: "restore save" finds it, not Back up now. ("rollback" is Other versions'.)
    'button:backup': 'sicherung sichern backup jetzt spielstand save',
    'button:restore': 'restore wiederherstellen zuruckholen zuruck backup backups sicherung save spielstand',
    'button:add_backup': 'backup sicherung datei file hinzufugen add zip',
    'button:import': 'import importieren ubernehmen transfer save spielstand charakter character shadps4 awaken '
                     'bbhost ps4 export',
    'button:trophies': 'erfolge trophaen trophies achievements ubernehmen importieren import transfer alte old '
                       'installation neuer new pc windows ssd zusammenfuhren merge platin verloren lost',
    'online_npid': 'npid online id konto account shadnet',
    'online_password': 'passwort password kennwort',
    'online_server': 'server adresse address shadnet ip',
    'online_webapi': 'webapi server',
    'online_community': 'spielserver community server nachrichten blutflecken geister messages',
    'thd_online_id': 'konto account discord anmelden hunters dream',
    'supermedo_npid': 'konto account supermedo npid',
    # the Online page's buttons ("log in" is joined to "login" in query_words); "melde" and "logge" for "wie melde
    # ich mich an", whose "an" is a stop word ("melden" alone is a report)
    'button:signin_supermedo': 'sign signin login einloggen anmelden anmeldung melde logge discord supermedo account '
                               'konto connexion connecter iniciar sesion entrar accedi zaloguj войти вход',
    'button:signin_password': 'sign signin login einloggen anmelden anmeldung passwort password kennwort online id '
                              'npid supermedo account konto connexion mot de passe contrasena senha haslo пароль войти',
    'button:signin_thd': 'sign signin login einloggen anmelden anmeldung melde logge discord hunters dream thd account '
                         'konto code connexion connecter iniciar sesion entrar accedi zaloguj войти вход',
    'button:signout': 'logout signout abmelden ausloggen account konto wechseln switch deconnexion выйти',
    'button:create_account': 'account konto registrieren registrierung register signup erstellen anlegen neu new '
                             'create supermedo email compte cuenta conta аккаунт',
    'button:test_connection': 'verbindung connection testen test prufen check ping erreichbar reachable offline '
                              'verbindungstest connexion conexion',
    'extra_env': 'umgebungsvariablen environment variablen env',
    'mouse:layout': 'dark souls ds3 wasd layout tastenbelegung keyboard tastatur maus',
    'mouse:stick': 'maus mouse steuert kamera bewegung stick',
    'mouse:camera': 'kamera camera maus mouse direkt raw input',
    'mouse:camera_sensitivity': 'empfindlichkeit sensitivity maus mouse dpi kamera schnell langsam',
    'mouse:speed': 'empfindlichkeit sensitivity maus mouse stick kamera schnell langsam',
    'mouse:speed_offset': 'drehgeschwindigkeit turn speed maus mouse stick',
    'mouse:sensitivity_y': 'vertikal vertical maus mouse hoch runter oben unten',
    'mouse:enabled': 'maus mouse look mouselook mausblick umsehen zielen aim f7 einschalten aktivieren enable',
    'mouse:invert_x': 'invert inverted inverse invertieren umkehren umgekehrt x-achse x-axis',
    'mouse:invert_y': 'invert inverted inverse invertieren umkehren umgekehrt y-achse y-axis',
    'button:reset_controller': 'reset zurucksetzen default defaults standard ursprunglich original',
    'auto_graphics': 'automatisch automatic empfohlen recommended einstellungen settings',
    'gpu_override': 'grafikkarte gpu graphics card erkannt',
    'page:controls': 'tasten tastatur maus controller gamepad belegung tastenbelegung steuerung keyboard mouse keys '
                     'bindings keybindings keybinds keybind remap rebind belegen xbox dualsense',
    'page:saves': 'spielstand spielstande save saves import shadps4 bbhost awaken backup sicherung charakter',
    'page:help': 'hilfe help problem fehler absturz crash',
    # Launcher customization (the page key stays 'language'): the launcher's own language, look and shortcuts;
    # the game's text, voices and sound are on Gameplay
    'page:language': 'launcher anpassen customization customize aussehen look sprache language transparent tray '
                     'verknupfung shortcut desktop steam discord',
    'page:gameplay': 'sprache language textsprache untertitel subtitles stimmen voices sprachausgabe synchro ton '
                     'sound audio lautstarke volume stumm mute intro tweaks anderungen spielername',
    'action:setup': 'einrichtung ersteinrichtung setup assistent guide wizard erster start first start neu '
                    'wiederholen repeat again reset zurucksetzen',
    # the side bar's own buttons
    'button:expert': 'experte expertenansicht expertenmodus expert ansicht view modus mode einfach simple mehr more '
                     'alle all optionen options einstellungen settings',
    'button:site': 'website webseite homepage seite site faq guide anleitung install installation installieren',
    # Help's last card and the Picture page
    'button:copy_summary': 'kopieren copy zusammenfassung summary diagnose diagnostics zwischenablage clipboard specs',
    'button:show_log': 'log logs protokoll ausgabe output',
    'button:undo': 'ruckgangig zuruck revert vorher before previous empfehlung recommended',
    'page:trophies': 'erfolge trophaen trophies achievements platin',
    'page:online': 'online koop coop multiplayer freunde friends friend hunters dream shadnet anmelden discord',
    'page:mods': 'reihenfolge order ziehen drag drop mods mod remaster patches add install hinzufugen installieren',
    'page:system': 'pc hardware grafikkarte prozessor anforderungen requirements system',
    'page:picture': 'grafik bild leistung fps ziel flussig ausgewogen leise laptop akku',
    'page:graphics': 'grafik graphics alle einstellungen',
    'page:cheats': 'cheats unsterblich god mode tricks debug',
    'page:advanced': 'erweitert update updates aktualisieren versionen diagnose diagnostics',
    'page:log': 'log protokoll ausgabe',
    'page:game': 'spieldateien spielordner ordner eboot dlc game files dump',
    'topic:page_file': 'absturz crash code 3 stürzt schliesst startet nicht speicher auslagerungsdatei ram guest memory',
    'topic:eboot_modified': 'eboot 30 60fps patch lance sauber clean gesten',
    'topic:eboot_truncated': 'eboot abgeschnitten out of bounds kaputt',
    'topic:eboot_unreadable': 'eboot lesen self',
    'topic:version': '1.09 version update 30',
    'topic:update_only': 'gxshader update ordner startet nicht',
    'topic:empty_files': 'leere dateien entpacken kaputt',
    'topic:antivirus': 'virenscanner antivirus defender winerror blockiert permission',
    'topic:location': 'onedrive programme program files ordner',
    'topic:vram': 'vram grafikspeicher ruckelt texturen gesichter memory',
    'topic:driver': 'treiber driver device lost absturz grafikkarte',
    'topic:shader_cache': 'ruckeln ruckelt stutter stuttering stutters lagging laggy erste minuten shader',
    'topic:save_lost': 'spielstand weg kaputt beschadigt charakter verschwunden verloren fortschritt save lost '
                       'corrupted damaged gone missing laden leer sicherung zuruckholen',
    # symptoms, and the device only as a word's start: a plain "controller" finds the Controls page first
    'topic:controller': 'controllers gamepads geht nicht funktioniert nicht reagiert nicht erkannt not working',
    'controller_test': 'controller gamepad test testen erkannt tasten knopfe funktioniert dualsense ps5 ps4 xbox '
                       'switch',
    'topic:smartscreen': 'smartscreen windows geschutzt schutz warnung',
    'topic:black_screen': 'schwarz schwarzer bildschirm black screen shader cache leeren',
    'topic:play_together': 'zusammen spielen freunde freund friends mitspielen coop co-op koop summon beschworen '
                           'glocke bell party host gast guest multiplayer seamless',
}
# With the game tests on (bbport_launcher_win.GAME_TESTS): their words, added to their pages.
GAME_TEST_WORDS = {'page:system': 'benchmark test messen stabilitat beta tests', 'page:picture': 'messen benchmark'}
# The words of a question around what is meant ("where is", "how do i", "wie kann ich"): every word
# has to be found, and these are in almost no entry, so a question found nothing; counted as found,
# they would find what starts with them ("im" in "Import", "mich" in a hint). ("die" stays: the
# English one is a word players mean; "mit" too: "spielen mit".)
STOP_WORDS = set('where is are how do does did i my me to the a an can what when why which it of in on for and '
                 'please pls wo ist sind wie mein meine meinen ich kann man was wann warum und der das den dem ein '
                 'eine es gibt im am zu zum zur beim vom mich mir dich'.split())
# Two words players write for one, joined before the stop words go ("in" is one; "log" alone is the Log
# page), and the launcher's own "sign-in" written by players.
JOINED = {'log in': 'login', 'logged in': 'login', 'log out': 'logout', 'log-in': 'login', 'log-out': 'logout',
          'sign-in': 'signin', 'sign-out': 'signout'}


def fold(text):
    """Lower case without accents (ä→a, ß→ss): what the player types matches either way."""
    text = str(text).casefold().replace('ß', 'ss')
    return ''.join(c for c in unicodedata.normalize('NFKD', text) if not unicodedata.combining(c))


def query_words(query):
    """The words of a query that say what is meant: without a question's marks, the stop words left
    out while others remain, a size like 2560x1440 read as "resolution"."""
    text = ' '.join(w.strip('?!.,;:"') for w in fold(query).split())
    for phrase, word in JOINED.items():
        text = re.sub(rf'\b{phrase}\b', word, text)
    words = ['resolution' if re.fullmatch(r'\d{3,5}x\d{3,5}', w) else w for w in text.split()]
    return [w for w in words if w not in STOP_WORDS] or words


def word_points(word, title, hint, extra):
    """How well one word finds an entry (its folded title, hint and players' words); None if not at all."""
    penalty = 0
    if word not in title and word not in extra and word not in hint:
        stem = near(word, title + ' ' + extra + ' ' + hint)
        if not stem:
            return None
        word, penalty = stem, 1  # a near form counts a little less than the word itself
    if f' {word} ' in f' {extra} ':
        points = 7  # one of the players' own words for it: what they mean, in any language
    elif f' {word}' in f' {title}':
        points = 6
    elif word in title:
        points = 4
    elif f' {word}' in f' {extra}':
        points = 3
    elif word in extra:  # inside a longer word ("scharfe" in "tiefenscharfe"): a little less
        points = 2
    elif word in hint:
        points = 1
    else:
        return None
    return points - penalty



def near(word, text):
    """A word as the text has it at the start of one of its words, without a plural or inflected
    ending ("details" -> "detail", "einstellungen" -> "einstellung"); None if there is none."""
    for cut in (1, 2):
        stem = word[:-cut]
        if len(stem) >= 5 and f' {stem}' in f' {text}':
            return stem
    return None


def shown(widget, page):
    """Whether a widget is laid out on its page (not in a part that is hidden for another choice)."""
    while widget is not None and widget is not page:
        try:
            if not widget.winfo_manager():
                return False
        except Exception:  # a widget that is gone
            return False
        widget = widget.master
    return True

class SearchMixin:
    """The search field, its index and the jump to a result."""

    # ---- the index -------------------------------------------------------------------------------
    def setting_key_of(self, widget):
        """The setting a row's widget edits: its setting_key, else the first setting's Tk variable in
        it or below it (an entry, a frame with a slider, radio buttons, the mouse settings)."""
        names = {str(var): key for key, var in getattr(self, 'vars', {}).items()}
        names.update({str(var): f'mouse:{key}' for key, var in getattr(self, 'mouse_vars', {}).items()})
        todo = [widget]
        while todo:
            item = todo.pop(0)
            key = getattr(item, 'setting_key', None)
            if key:
                return key
            for option in ('textvariable', 'variable'):
                try:
                    name = str(item.cget(option))
                except Exception:  # a widget without that option
                    continue
                if name in names:
                    return names[name]
            todo.extend(item.winfo_children())
        return None

    def index_entry(self, widget, title, hint='', key=None, mark=None, reveal=None):
        """A setting on a page: found by its title, its hint and WORDS[key]; `mark` is the widget
        marked after the jump (its label). `reveal` opens the part of the page it is folded into
        (found while folded; the jump unfolds it)."""
        if not hasattr(self, 'search_settings'):
            self.search_settings = []
        if title:
            self.search_settings.append({'kind': 'setting', 'widget': widget, 'mark': mark or widget,
                                        'title': str(title), 'hint': str(hint or ''), 'key': key, 'reveal': reveal})

    def index_only(self, widgets, note, key):
        """The settings in `widgets` show only for another choice (another online pool, another
        upscaler): they stay findable, the result says `note` ("only with …"), and the jump marks the
        setting `key`, where that choice is made. The choice itself stays as the player set it."""
        names = {str(widget) for widget in widgets}
        for entry in getattr(self, 'search_settings', []):
            widget = entry['widget']
            while widget is not None and str(widget) not in names:
                widget = widget.master
            if widget is not None:
                entry['only'] = (note, key)

    def hidden_for_now(self, entry):
        """A setting its page does not show now (a part hidden for another choice, without a reveal)."""
        return entry['kind'] == 'setting' and not entry.get('reveal') and not shown(
            entry.get('widget'), getattr(self, 'pages', {}).get(entry['page']))

    def search_ready(self):
        """After every page is built: which page each setting is on, then the pages and the Help topics."""
        import bbport_help
        titles = dict((name, _(*title)) for name, title in L.NAV_SIMPLE + L.NAV_EXPERT)
        titles['log'] = _('Log', 'Журнал')
        self.page_titles = titles
        pages = {str(frame): name for name, frame in self.pages.items()}
        if getattr(self, 'side', None) is not None:
            pages[str(self.side)] = 'side'  # the side bar's own buttons: the expert view, the website
        entries = []
        for entry in getattr(self, 'search_settings', []):
            widget, page = entry['widget'], None
            while widget is not None and page is None:
                page = pages.get(str(widget))
                widget = widget.master
            if page:
                entry['page'] = page
                if page == 'side':
                    entry['kind'] = 'side'
                entries.append(entry)
        for name, title in titles.items():
            entries.append({'kind': 'page', 'page': name, 'title': title, 'hint': '', 'key': f'page:{name}'})
        for key in bbport_help.TOPIC_ORDER:
            title, why, _steps = bbport_help.TOPICS[key]
            entries.append({'kind': 'topic', 'page': 'help', 'topic': key, 'title': _(title), 'hint': _(why),
                            'key': f'topic:{key}'})
        entries.append({'kind': 'action', 'page': 'help', 'run': 'open_setup', 'title': _('Repeat the setup'),
                        'hint': _('Set everything up again, step by step.'), 'key': 'action:setup'})
        for entry in entries:
            words = WORDS.get(entry.get('key') or '', '')
            if getattr(L, 'GAME_TESTS', True):
                words += ' ' + GAME_TEST_WORDS.get(entry.get('key') or '', '')
            entry['haystack'] = (fold(entry['title']), fold(entry['hint']), fold(words))
        self.search_entries = entries

    def search(self, query, limit=7):
        """The best entries for a query: those with every word, the title counting most; when none has
        every word, those with the most of them (a question with a word the launcher does not use)."""
        words = query_words(query)
        if not words:
            return []
        whole = ' '.join(fold(query).split())
        scored = []
        for entry in self.search_entries:
            title, hint, extra = entry['haystack']
            points = [p for p in (word_points(word, title, hint, extra) for word in words) if p is not None]
            score = sum(points)
            if not score:
                continue
            later = self.hidden_for_now(entry)  # shown for another choice: after what the page shows now
            if later and not entry.get('only'):
                continue
            if whole == ' '.join(title.split()):
                score += 3  # the whole title typed: that one, before longer titles with the same words
            # symptoms: the fix first
            bonus = {'setting': 0.5, 'topic': 0.6, 'page': 0.3, 'action': 0.6, 'side': 0.5}[entry['kind']]
            expert = entry['page'] in L.EXPERT_PAGES
            scored.append((len(points), later,
                           score + bonus - (0.2 if expert and self.app.get('ui_mode') != 'expert' else 0), entry))
        if any(found == len(words) for found, _later, _score, _entry in scored):
            scored = [item for item in scored if item[0] == len(words)]
        scored.sort(key=lambda item: (-item[0], item[1], -item[2]))
        seen, out = set(), []
        for _found, _later, _score, entry in scored:
            ident = (entry['kind'], entry['page'], entry['title'])
            if ident not in seen:
                seen.add(ident)
                out.append(entry)
            if len(out) == limit:
                break
        return out

    # ---- the field and its results ----------------------------------------------------------------
    def build_search(self, side):
        tk, px = self.tk, self.px
        box = tk.Frame(side, bg=theme.LINE if hasattr(theme, 'LINE') else theme.BORDER, padx=1, pady=1)
        box.pack(fill='x', padx=px(14), pady=(0, px(10)))
        inner = tk.Frame(box, bg=theme.DEEP)
        inner.pack(fill='x')
        tk.Label(inner, text='', bg=theme.DEEP, fg=theme.MUTED, font=(theme.ICONS, 10)).pack(
            side='left', padx=(px(9), px(4)))
        self.search_var = tk.StringVar()
        entry = tk.Entry(inner, textvariable=self.search_var, bg=theme.DEEP, fg=theme.TEXT_STRONG, relief='flat', bd=0,
                         insertbackground=theme.TEXT_STRONG, font=(theme.BODY, 10), highlightthickness=0)
        entry.pack(side='left', fill='x', expand=True, ipady=px(6))
        hint = tk.Label(inner, text=_('Search…') + '   ' + _('Ctrl+F'), bg=theme.DEEP, fg=theme.MUTED,
                        font=(theme.BODY, 9))
        hint.place(in_=entry, x=0, rely=0.5, anchor='w')
        self.search_box, self.search_field, self.search_hint = box, entry, hint
        self.search_popup, self.search_results, self.search_pick = None, [], 0

        def placeholder(*_a):
            if self.search_var.get() or self.root.focus_get() is entry:
                hint.place_forget()
            else:
                hint.place(in_=entry, x=0, rely=0.5, anchor='w')
        self.search_var.trace_add('write', lambda *_a: (placeholder(), self.search_update()))
        entry.bind('<FocusIn>', lambda _e: (placeholder(), box.configure(bg=theme.GOLD), self.search_update()))
        entry.bind('<FocusOut>', lambda _e: (placeholder(), box.configure(bg=theme.BORDER),
                                             self.root.after(180, self.search_close)))
        hint.bind('<Button-1>', lambda _e: entry.focus_set())
        entry.bind('<Down>', lambda _e: self.search_move(1))
        entry.bind('<Up>', lambda _e: self.search_move(-1))
        entry.bind('<Return>', lambda _e: self.search_go())
        entry.bind('<Escape>', lambda _e: (self.search_var.set(''), self.search_close(), self.root.focus_set()))
        self.root.bind_all('<Control-f>', self.search_focus)
        box.configure(bg=theme.BORDER)

    def search_focus(self, _event=None):
        """Ctrl+F: the search field; not while a key is being bound on the Controls page (Ctrl+F
        can be a binding) or while the setup guide covers the window."""
        if getattr(self, 'remap_listening', False) or getattr(self, 'guide', None):
            return None
        self.search_field.focus_set()
        self.search_field.select_range(0, 'end')
        return 'break'

    def search_update(self):
        query = self.search_var.get().strip()
        self.search_results = self.search(query) if query else []
        self.search_pick = 0
        if not self.search_results:
            self.search_close()
            if query:
                self.search_show([None])
            return
        self.search_show(self.search_results)

    def search_where(self, entry):
        page = self.page_titles.get(entry['page'], entry['page'])
        if entry['kind'] == 'topic':
            where = _('Help') + ' · ' + _('problem')
        elif entry['kind'] == 'page':
            where = _('Page')
        elif entry['kind'] == 'side':
            where = _('Side bar')
        else:
            where = page
        if entry['page'] in L.EXPERT_PAGES and self.app.get('ui_mode') != 'expert':
            where += ' · ' + _('expert view')
        if entry.get('only') and self.hidden_for_now(entry):
            where += ' · ' + entry['only'][0]
        return where

    def search_show(self, results):
        tk, px = self.tk, self.px
        if not self.search_popup:
            popup = tk.Toplevel(self.root)
            popup.overrideredirect(True)
            popup.attributes('-topmost', True)
            popup.configure(bg=theme.GOLD)
            self.search_popup = popup
        popup = self.search_popup
        for child in popup.winfo_children():
            child.destroy()
        frame = tk.Frame(popup, bg=theme.CARD, padx=px(4), pady=px(4))
        frame.pack(fill='both', expand=True, padx=1, pady=1)
        self.search_rows = []
        for index, entry in enumerate(results):
            row = tk.Frame(frame, bg=theme.CARD, padx=px(8), pady=px(5), cursor='hand2')
            row.pack(fill='x')
            if entry is None:
                tk.Label(row, text=_('Nothing found. Try other words, or open Help.'), bg=theme.CARD, fg=theme.MUTED,
                         font=(theme.BODY, 9)).pack(anchor='w')
                row.bind('<Button-1>', lambda _e: (self.search_close(), self.show('help')))
                continue
            title = tk.Label(row, text=entry['title'], bg=theme.CARD, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 10),
                             anchor='w', justify='left', wraplength=px(330))
            title.pack(anchor='w', fill='x')
            where = tk.Label(row, text=self.search_where(entry), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 8),
                             anchor='w', justify='left', wraplength=px(330))  # with "only with …" it can be long
            where.pack(anchor='w')
            for widget in (row, title, where):
                widget.bind('<Button-1>', lambda _e, i=index: (setattr(self, 'search_pick', i), self.search_go()))
                widget.bind('<Enter>', lambda _e, i=index: self.search_mark(i))
            self.search_rows.append((row, title, where))
        self.search_mark(self.search_pick)
        field = self.search_field
        x = field.winfo_rootx() - px(30)
        y = self.search_box.winfo_rooty() + self.search_box.winfo_height() + px(4)
        popup.update_idletasks()
        popup.geometry(f'{px(370)}x{frame.winfo_reqheight() + 2}+{x}+{y}')
        popup.deiconify()

    def search_mark(self, index):
        self.search_pick = index
        for i, (row, title, where) in enumerate(getattr(self, 'search_rows', [])):
            color = theme.SURFACE if i == index else theme.CARD
            for widget in (row, title, where):
                widget.configure(bg=color)

    def search_move(self, step):
        if self.search_results:
            self.search_mark((self.search_pick + step) % len(self.search_results))
        return 'break'

    def search_close(self):
        if self.search_popup and (self.root.focus_get() is not self.search_field or
                                  not self.search_var.get().strip()):
            self.search_popup.destroy()
            self.search_popup = None

    def search_go(self):
        if not self.search_results:
            return 'break'
        entry = self.search_results[min(self.search_pick, len(self.search_results) - 1)]
        self.search_var.set('')
        if self.search_popup:
            self.search_popup.destroy()
            self.search_popup = None
        self.root.focus_set()
        self.jump(entry)
        return 'break'

    def jump(self, entry):
        """Opens a result: its page, scrolled to it and marked for a moment (a Help topic opens)."""
        if entry['kind'] == 'topic':
            self.open_topic(entry['topic'])
            return
        if entry['kind'] == 'action':
            getattr(self, entry['run'])()
            return
        if entry['kind'] == 'side':  # always in view: only marked
            self.root.after(60, lambda: self.flash(entry['mark'], entry['widget']))
            return
        if entry.get('only') and self.hidden_for_now(entry):
            # shown for another choice: to where that choice is made, not changing it for the player
            entry = next((e for e in self.search_entries if e.get('key') == entry['only'][1] and
                          e['page'] == entry['page'] and not self.hidden_for_now(e)), entry)
        self.show(entry['page'])  # an expert page turns the expert view on (Launcher.show)
        if entry['kind'] == 'setting':
            if entry.get('reveal') and not shown(entry['widget'], self.pages.get(entry['page'])):
                entry['reveal']()
            self.root.update_idletasks()
            self.scroll_to(entry['mark'], above=self.px(110))  # some of what is above stays in view
            self.root.after(60, lambda: self.flash(entry['mark'], entry['widget']))

    def scroll_to(self, widget, above=None):
        tk = self.tk
        inner = widget
        while inner.master is not None and not isinstance(inner.master, tk.Canvas):
            inner = inner.master
        canvas = inner.master
        if canvas is None:
            return
        try:
            top = widget.winfo_rooty() - inner.winfo_rooty()
            canvas.yview_moveto(max(0, top - (above if above is not None else self.px(70))) /
                                max(1, inner.winfo_height()))
        except tk.TclError:
            pass

    def flash(self, mark, widget=None):
        """Here it is: a gold frame around the setting's whole row (its title, its control, the hint
        under it) with a bar at its left, the title in gold; after a moment it fades into the page."""
        tk, px = self.tk, self.px
        for part in getattr(self, 'flash_parts', []):  # the one before, if it still shows
            part.destroy()
        if getattr(self, 'flash_undo', None):
            self.flash_undo()
        self.flash_parts, self.flash_undo = [], None
        parent = mark.master
        try:
            items = [mark] + ([widget] if widget is not None and widget is not mark and widget.master is parent else [])
            info = mark.grid_info()
            if info:  # the hint under a row (row() and check() grid it in the next row)
                below = parent.grid_slaves(row=int(info['row']) + 1)
                items += [w for w in below if w.winfo_class() == 'TLabel' and str(w.cget('style')) == 'Muted.TLabel']
            parent.update_idletasks()
            top = min(w.winfo_y() for w in items) - px(7)
            bottom = max(w.winfo_y() + w.winfo_height() for w in items) + px(7)
            left = max(0, min(w.winfo_x() for w in items) - px(14))
            # a row of a page reaches across the page; anything else as wide as itself
            right = parent.winfo_width() - px(14) if info else max(w.winfo_x() + w.winfo_width() for w in items) + px(14)
            try:
                page = self.ttk.Style().lookup(str(parent.cget('style')) or parent.winfo_class(), 'background')
            except tk.TclError:
                page = ''
            page = page or theme.BG
        except (tk.TclError, ValueError):
            return
        width, height = right - left, bottom - top
        lines = [(left, top, width, px(2)), (left, bottom - px(2), width, px(2)), (right - px(2), top, px(2), height),
                 (left, top, px(4), height)]  # top, bottom, right; the bar at the left
        for x, y, w, h in lines:
            part = tk.Frame(parent, bg=theme.GOLD_LIGHT, width=w, height=h)
            part.place(x=x, y=y, width=w, height=h, bordermode='outside')  # from the edge, as winfo_x counts
            part.lift()
            self.flash_parts.append(part)
        # the title in gold: a style of its own for the moment (the theme's maps outrank a widget's colour)
        titled = mark.winfo_class() in ('TLabel', 'TCheckbutton')
        style, old = self.ttk.Style(), ''
        if titled:
            old = str(mark.cget('style'))
            flashed = f'Flash.{old or mark.winfo_class()}'

            def title(color):
                style.configure(flashed, foreground=color)
                style.map(flashed, foreground=[('!disabled', color)])
            title(theme.GOLD_LIGHT)
            mark.configure(style=flashed)
            self.flash_undo = lambda: mark.winfo_exists() and mark.configure(style=old)
        parts = list(self.flash_parts)

        def fade(step, steps=14):
            if not all(p.winfo_exists() for p in parts):
                return
            if step > steps:
                for p in parts:
                    p.destroy()
                if titled and mark.winfo_exists():
                    mark.configure(style=old)
                return
            color = theme.blend(theme.GOLD_LIGHT, page, step / steps)
            for p in parts:
                p.configure(bg=color)
            if titled and mark.winfo_exists():
                title(theme.blend(theme.GOLD_LIGHT, theme.TEXT, step / steps))
            self.root.after(70, lambda: fade(step + 1))
        self.root.after(1300, lambda: fade(1))
