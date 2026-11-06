# SPDX-License-Identifier: GPL-2.0-or-later
"""What's new: shown over the launcher at its first start in a new version (VERSION_TEXT, so every beta
counts), and again from Help > What's new (and the Play page's "New in version" line until seen).
NOTES holds each version's notice (what players must know or do, in every language: bbport_lang_news) and its changes (in English, like the
release notes on GitHub). A version without notes shows nothing at start."""
import bbport_theme as theme

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


def notes(version):
    """(notice, [(section, [change, ...]), ...]) of a version, or None."""
    return NOTES.get(version)


def servers_notice():
    return _('Online servers: the port now plays on its own servers. SuperMedo Server is the default and '
             "recommended; The Hunter's Dream stays available for any players who want to use their server. "
             'Other servers can no longer be chosen: if you used one, you now play on SuperMedo Server. Sign '
             'in, or make a free account, on the Online page.')


def hunters_dream_notice():
    return _("The Hunter's Dream is no longer offered: the port plays on SuperMedo Server only. If you played there, "
             'you now play on SuperMedo Server. Sign in, or make a free account, on the Online page.')


NOTES = {
    '1.9.8.1': (
        None,
        [('Seamless co-op', [
            'Seamless co-op starts again with mods whose event files hold an empty event: it stopped with '
            '"Making the co-op event files failed".',
        ])]),
    '1.9.8': (
        None,
        [('Seamless co-op (new)', [
            "Seamless co-op is here: BBSC 1.0.0 (Yharnam Together) on its own Seamless co-op page under Online "
            "play, on SuperMedo's seamless server with your SuperMedo account. Up to 5 hunters stay together "
            'through deaths, lamps and areas.',
            "It runs on this port's own engine with every setting of this launcher: picture, frame rate, controls, "
            'mods and patches. The normal game and normal online play are unchanged.',
            "BBSC's co-op screens are in the game: the team screen at the title (F2 in the game), the party HUD, "
            'the compass, team pings (T), the map (M), trades, duels and travelling together. On a controller, '
            'hold View/Back (the touchpad on PlayStation) for the map and tap it twice for a ping.',
            "Each teammate's character picture shows in the party list, on the compass and on the map.",
            "The team plays the host's world and its progress stays in the host's save. A player who joins is "
            'placed next to the host; a new character joining late gets the starter weapons.',
            'Host rules: enemy difficulty with a Custom choice (your own health, damage and poise, 1 to 10), a '
            'Revival switch, boss tempo, Rot and travelling together.',
            'Seamless co-op keeps its own save folder (seamless\\user); a new game is recommended.',
        ]), ('Parties and online', [
            'Parties: make a party code on the Online page and send it to your friends. In a party, your summon '
            "signs and Beckoning Bell find only your party's hunters, with no level limit.",
            'Sign in to SuperMedo Server with your Online ID and password, or make a new account, not only with '
            'Discord.',
            'Cheat and debug patches are left out of online games; with one on, Play asks what to do.',
            'Back online by itself after a dropped connection; a player who joins again is no longer kicked; '
            "players who needed a VPN join through the server's relay; no more short freeze about once a minute.",
        ]), ('Performance and fixes', [
            'Faster locks inside the engine: a few more frames per second. Three rare freezes fixed.',
            'Low latency is Auto by default: NVIDIA Reflex and Boost on NVIDIA cards, off on the others.',
        ]), ('Trophies, saves and patches', [
            'A pop-up shows each trophy as you earn it, with its own picture and a short chime.',
            'Characters deleted in the game no longer show in the launcher; named backups are kept.',
            'Every patch in the list is from the public community list or written for this port.',
        ])]),
    '1.9.8 beta 14': (
        None,
        [('Online', [
            "Seamless co-op: the Doll's level-up message shows only once, as in the normal game, and her talk "
            'no longer gets stuck after a level up (she sat down and nothing happened until you travelled).',
            'Seamless co-op start message: Seamless co-op keeps its own save folder; a new game is recommended, and '
            'main-game saves can be put into the seamless folder by hand at your own risk.',
            'Seamless co-op no longer closes with a "GPU library assertion failed" error (code 23) when the game '
            'hands the graphics an unreadable shader: that one draw is skipped instead.',
            "Seamless co-op keeps its own co-op event files over a mod's (MOAL's turned floors red and broke co-op "
            "events): such a mod's event files are not used in seamless, its other files are.",
        ])]),
    '1.9.8 beta 13': (
        None,
        [('Online', [
            'Seamless co-op host rules: Enemies has a Custom choice with your own health, damage and poise '
            "multipliers (1 to 10), and a Revival switch: off, a fallen hunter dies for real instead of going down.",
            "The Doll's level-up message no longer comes back at every talk in Seamless co-op, and leaving her talk "
            'quickly no longer leaves it stuck.',
        ])]),
    '1.9.8 beta 12': (
        None,
        [('Online', [
            "Seamless co-op plays the host's world: every time the team starts, its world is made from the host's "
            "own save, and the progress stays in the host's save. Guests get their own world back when they play "
            'alone.',
            'A player who joins the team is placed next to the host, a new character too.',
            'Going back to the title and loading another save no longer stops players joining each other until '
            'the game is restarted.',
            'A new character who joins a team late gets the starter weapons (Saw Cleaver, Hunter Axe, Threaded '
            'Cane, Hunter Pistol and Hunter Blunderbuss).',
            'Seamless co-op plays at your own frame rate setting, as the normal game does.',
            'Starting Seamless co-op reminds you to use a new character, not your main save.',
        ])]),
    '1.9.8 beta 11': (
        None,
        [('Online', [
            "Seamless co-op shows each teammate's character picture in the party list, on the compass and on the "
            'map. Your picture is taken from your hunter as they look now, and taken again when you change outfit.',
        ]), ('Patches', [
            'Every patch in the patch list is now from the public community list or written for this port. The '
            "Hunter's Dream live-map patch is replaced by this port's own Faster play log patch.",
        ])]),
    '1.9.8 beta 10': (
        None,
        [('Online', [
            'Seamless co-op on a controller: hold View/Back (the touchpad on a PlayStation pad) for the map, tap it '
            'twice for a ping. The map and the co-op team are also in the game menu, under System.',
            "Seamless co-op runs as a release build, as BBSC's own releases do.",
        ])]),
    '1.9.8 beta 9': (
        None,
        [('Online', [
            "Seamless co-op runs on this port's own engine now (bin\\bb-probe-seamless.exe, built with BBSC "
            "1.0.0's co-op), started by this launcher with every one of its settings: picture, controls, mods and "
            'patches. The normal game and its engine are unchanged.',
            "BBSC's co-op screens are in the game: the team screen at the title (your team, invites, quick match, "
            'the world, Begin the hunt or Alone; F2 opens it in the game), the party HUD, the compass, team pings '
            '(T), the map (M), trades, duels and travelling together.',
        ])]),
    '1.9.8 beta 8': (
        None,
        [('Online', [
            "Seamless co-op is BBSC 1.0.0 (BBSC: Yharnam Together) now, on its own Seamless co-op page under Online "
            "play: it plays on SuperMedo's seamless server with your SuperMedo account, and your party is your co-op "
            'team. The host rules (enemies, boss tempo, Rot, travelling together) are on that page; the picture '
            'follows yours. The normal online game is as before.',
            "BBSC 1.0.0 keeps its characters in its own folder (seamless\\user): the earlier Seamless co-op's "
            'characters are not moved there. Everyone in a seamless session needs this version.',
        ])]),
    '1.9.8 beta 7': (
        None,
        [('Trophies', [
            'A pop-up shows each trophy as you earn it, with its own picture from your game and a short chime '
            '(BB_TROPHY_SOUND=0 turns the chime off, BB_TROPHY_POPUPS=0 the pop-up).',
        ]), ('Performance', [
            'Faster locks inside the engine: a few more frames per second, and a rare freeze between two locks is '
            'gone.',
            'Low latency is Auto by default: NVIDIA Reflex and Boost on NVIDIA cards, off on the others. Its '
            'settings are translated now.',
        ]), ('Game and saves', [
            'Characters deleted in the game no longer show in the launcher.',
            "The game files check's list of the 1.09 files is fixed.",
            'Windows: every open game file keeps its own read position.',
            'For crash reports: BB_GPU_CHECKPOINTS=1 records where the GPU was when it stopped (code 23 crashes). '
            'Off by default.',
        ])]),
    '1.9.8 beta 6': (
        None,
        [('Online', [
            'Sign in to SuperMedo Server with your Online ID and password, or make a new account, not only with '
            'Discord: on the Online page, at Play, and in the setup guide.',
            'Cheat and debug patches are left out of online games (Seamless co-op too). With one on, Play asks: online '
            'without it, or this one start offline with it.',
            'Seamless co-op: new difficulty steps (Normal is gentler now), the party box shows there too, and the '
            'Online page says who hosts and when new characters get the bells.',
            'Parties take 5 hunters now, for Seamless co-op with 4 guests.',
            'Seamless co-op: dead and watching a hunter, your unseen body now goes with them, so their area stays '
            'loaded and you see them, also far from where you fell.',
            "Seamless co-op: a guest who dies while the host fights a boss now watches the fight instead of waking "
            'at the lamp (when the boss bar had not come up on their own screen).',
            'Co-op: a player whose game dropped and who joins again is no longer kicked by the host right away '
            '(the host kept the old, dead connection to them). They show up again.',
            'Online: connections to the servers are kept and reused, so each call waits one trip instead of two '
            'or more. Far from the servers this ends the short freeze about once a minute.',
            "Co-op: players behind their provider's shared address (they needed a VPN) join through the server's "
            'relay when only one way gets through, instead of failing.',
            "A character played in Seamless co-op gets the game's own bells back when it is played normally (they "
            "showed as ?GoodsName? and did nothing), and BBSC's items have names in every language.",
        ]), ('Saves', [
            "Seamless co-op's characters and BBSC's own backups are never offered to your normal saves.",
            'Automatic backups you named are kept, and a backup added from a file no longer stops the next one.',
        ])]),
    '1.9.8 beta 5': (
        None,
        [('Online', [
            "Seamless co-op: BBSC's own direct channel between players works now. The port refused it, so warps, "
            "world sync and the host's progress went the long way through the server's relay, late.",
            'Seamless co-op settings on the Online page: the difficulty (how much stronger enemies and bosses get '
            "for every guest) and BBSC's main options. The host's settings count for the whole session.",
            "Seamless co-op's bells are given once you reach the Hunter's Dream, not at the very start.",
            "Looking up another player by name no longer fails on SuperMedo's servers.",
        ])]),
    '1.9.8 beta 4': (
        None,
        [('Online', [
            'Seamless co-op has its own characters again. Beta 3 played your normal ones, and Seamless co-op '
            'swaps their bells for its own items: back in the normal game they showed as "?GoodsName?" and the '
            'bells were gone. If that happened to you, put back a backup from before you played Seamless co-op '
            '(Saves page).',
            'The Saves page follows your choice: with Seamless co-op chosen it shows, backs up and restores '
            "Seamless co-op's characters, and offers your normal characters to copy in.",
            "Parties: a box beside the party code shows who is in your party, with each hunter's level and who "
            'is online. It updates by itself.',
            'Test connection no longer says the Seamless co-op game server is not reachable.',
        ])]),
    '1.9.8 beta 2': (
        None,
        [('Online', [
            "SuperMedo Seamless co-op (beta): BBSC's seamless co-op on SuperMedo's own seamless server, with your "
            'SuperMedo account. Choose it on the Online page; everyone in a session picks it.',
            'Parties: make a party code on the Online page and send it to your friends. While you are in a party, '
            "your summon signs and your Beckoning Bell find only your party's hunters, with no level limit.",
            'Back online by itself after a dropped connection, within a minute.',
        ]), ('Fixes', [
            'Two rare freezes fixed (the game could hang for good while loading areas or adding new shaders).',
        ])]),
    '1.9.7': (
        hunters_dream_notice,
        [('Online', [
            "The Hunter's Dream is no longer offered: SuperMedo Server is the port's server. Players who chose The "
            "Hunter's Dream move to SuperMedo Server by themselves.",
        ]), ('Fixes', [
            'Updates install safely: the new version is put in place in one step and undone when Windows holds a '
            'file, so an update can no longer leave a launcher that does not start ("Failed to start embedded '
            'python interpreter").',
            'Restart Game in PC Settings keeps you online: the game came back signed out before.',
            'Graphics for your PC: no sharpening on top of FSR 4 (it left flaws along edges), and a new graphics '
            'card gets its own upscaler (DLSS on a GeForce RTX). The frame rate you chose stays.',
            'Saves and settings are written in one piece, so a crash or a full disk cannot cut them in half.',
            'Starting from the desktop shortcut or Steam without an online account plays offline instead of '
            'failing.',
        ]), ('Checks before you play', [
            'The launcher tells you when Windows sees a graphics card that Vulkan does not, when the game folder '
            'is in OneDrive, when a driver is too old, when a runtime for AMD\'s FSR 4 is missing, and when the '
            'firewall blocks the game.',
            'A changed eboot.bin is swapped for a clean copy found on your PC, and Help can check every game file.',
        ]), ('Launcher', [
            'Achievements can be brought over from another installation.',
            'Help asks on our forum now: Help & Support on forum.bbport.de. The Discord invite works again.',
            'Search finds more of what players ask for, and the side bar fits small windows.',
        ])]),
    '1.9.6': (
        None,
        [('Fixes', [
            'Online: "Failed to connect to Bloodborne server" at Play Online on some PCs is fixed. The port now '
            "carries Let's Encrypt's root certificates itself, so it trusts SuperMedo Server and The Hunter's "
            "Dream even before Windows has downloaded them, and Windows' own check gets the server's whole "
            'certificate chain.',
        ])]),
    '1.9.5': (
        servers_notice,
        [('Online', [
            'SuperMedo Server, our own online servers, is the default: sign in with Discord or make a free '
            "account on the Online page. The Hunter's Dream stays available.",
            'Co-op through a relay when routers block a direct connection, and over the LAN for players behind '
            'the same connection.',
        ]), ('Gameplay at any frame rate', [
            'Capes and cloth hang as on the PS4 and no longer spaz out when the frame rate stutters.',
            "Plants sway with the time that passed: no more jolting or rushing grass (Hemwick's lavender).",
            'Jumps go as far at any frame rate as on the PS4.',
            "Ebrietas: no more particle pile-up above 60 FPS, and her arena no longer loses its map and "
            'textures.',
        ]), ('Crashes and fixes', [
            'The most common random crash (ucrtbase.dll, often after about an hour) is fixed.',
            'Error 21 at start (memory in use by another program) no longer stops the game.',
            'Rebirth at the Doll: no T-pose, free levels again, and leaving the level-up screen undoes it.',
            'Common mistakes repaired for you: read-only saves, saves in the wrong folder, antivirus locks, '
            'game files from a PKG tool.',
        ]), ('New', [
            "PC Settings in the game's own System menu, for the controller.",
            'Low latency (Reflex, Anti-Lag 2), VRR, and new shaders built in the background: All graphics > '
            'Frame rate.',
            'FPS counter: choose its lines and colours (All graphics > FPS counter).',
            'Discord status with your area, boss and level; Discord and website buttons in the side bar.',
            'FSR 4.1.2 and ML frame generation 4.0.2 on AMD (experimental).',
            "This window: Help > What's new opens it again.",
        ])]),
    '1.9.5 beta 10': (
        servers_notice,
        [('Fixes', [
            'Ebrietas: the big frame drops in her fight are gone. Above 60 FPS (Uncap FPS++ and 90 FPS++), '
            'effects now step in whole 1/60 s, so her particles no longer pile up.',
            'Ebrietas: her arena no longer loses its map and textures until a restart, when the game leaves '
            'a table empty for a moment.',
            'Launcher: opening it from the tray no longer closes it.',
            'Object motion vectors: shown as unavailable, with the reason, on RX 6000 and older Radeons and on '
            'cards under 6 GB, where the game runs without them anyway.',
        ]), ('New options (All graphics > Frame rate, all off by default)', [
            'Low latency: NVIDIA Reflex on GeForce, AMD Anti-Lag 2 on Radeon.',
            'Variable refresh rate for G-Sync and FreeSync displays.',
            'Send frames to the GPU in parts: about 2 ms less lag (not on AMD Radeon, where it is slower).',
            'New shaders while playing: built in the background, without stutter.',
        ]), ('Also', [
            'Faster starts: saved shaders are built on all CPU cores, and the driver keeps its own compiled '
            'shaders between starts.',
            'Fresher input: the controller is read as the game asks for it, keyboard and mouse every '
            'millisecond.',
            'FPS counter: choose its lines (FPS, frame time, render latency, VRAM, GPU, CPU, RAM, graph) in All '
            'graphics > FPS counter or the in-game menu. Render latency has its own line, off by default. '
            'Value colours: colour-coded (green, orange, red) or plain grey.',
            "Online: only SuperMedo Server and The Hunter's Dream to choose from.",
            "This window: what has changed, at the first start of each new version; Help > What's new opens "
            'it again.',
            'Our Discord and website: two buttons at the bottom of the side bar. Come say hi!',
            'The low latency options in every launcher language.',
        ])]),
}


class NewsPanel:
    """Over the whole window below the title bar, like the setup guide: a card with the version's notice
    and changes. Continue (Enter / Esc) closes it; it counts as seen once shown."""

    def __init__(self, app, version):
        self.app, tk, ttk, px = app, app.tk, app.ttk, app.px
        notice, sections = notes(version)
        self.frame = tk.Frame(app.root, bg=theme.DEEP)
        top = app.titlebar_height() if hasattr(app, 'titlebar_height') else 0
        self.frame.place(x=0, y=top, relwidth=1, relheight=1, height=-top)
        self.frame.lift()
        card = tk.Frame(self.frame, bg=theme.CARD, highlightthickness=1, highlightbackground=theme.BORDER_STRONG)
        card.place(relx=0.5, rely=0.5, anchor='center', width=px(760), relheight=0.88)

        head = tk.Frame(card, bg=theme.CARD)
        head.pack(fill='x', padx=px(28), pady=(px(24), px(10)))
        tk.Label(head, text=_("WHAT'S NEW"), bg=theme.CARD, fg=theme.GOLD, font=(theme.HEAD, 9)).pack(anchor='w')
        tk.Label(head, text=_('Version {}').format(version), bg=theme.CARD, fg=theme.TEXT_STRONG,
                 font=(theme.HEAD_BOLD, 19)).pack(anchor='w')

        bar = tk.Frame(card, bg=theme.CARD)
        bar.pack(side='bottom', fill='x', padx=px(28), pady=(px(12), px(22)))
        ttk.Button(bar, text=_('Continue'), style='Accent.TButton', command=self.close).pack(side='right')
        page = L.BETA_PAGE if L.BETA else L.RELEASES_PAGE
        ttk.Button(bar, text=_('Full notes on GitHub'), command=lambda: L.webbrowser.open(page)).pack(
            side='right', padx=(0, px(8)))

        if notice:
            box = tk.Frame(card, bg=theme.SURFACE, highlightthickness=1, highlightbackground=theme.GOLD)
            box.pack(fill='x', padx=px(28), pady=(0, px(12)))
            tk.Label(box, text='ⓘ  ' + _('Important'), bg=theme.SURFACE, fg=theme.GOLD_LIGHT,
                     font=(theme.BODY_SEMI, 10)).pack(anchor='w', padx=px(14), pady=(px(10), 0))
            tk.Label(box, text=notice(), bg=theme.SURFACE, fg=theme.TEXT, font=(theme.BODY, 10), justify='left',
                     wraplength=px(680)).pack(anchor='w', padx=px(14), pady=(px(4), px(12)))

        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True, padx=(px(28), px(12)))
        text = tk.Text(body, bg=theme.CARD, fg=theme.TEXT, relief='flat', bd=0, highlightthickness=0, wrap='word',
                       font=(theme.BODY, 10), padx=0, pady=0, cursor='arrow', spacing1=px(2), spacing3=px(4))
        scroll = ttk.Scrollbar(body, orient='vertical', command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        text.pack(side='left', fill='both', expand=True)
        text.tag_configure('section', font=(theme.BODY_SEMI, 11), foreground=theme.GOLD_LIGHT,
                           spacing1=px(10), spacing3=px(4))
        text.tag_configure('item', lmargin1=px(4), lmargin2=px(20))
        for index, (section, items) in enumerate(sections):
            text.insert('end', ('\n' if index else '') + section + '\n', 'section')
            for item in items:
                text.insert('end', '•  ' + item + '\n', 'item')
        text.configure(state='disabled')  # read only (still scrolls and selects)

        for key in ('<Escape>', '<Return>'):  # on the panel, which has the focus: the launcher's own stay
            self.frame.bind(key, lambda _e: self.close())
        self.frame.focus_set()
        app.app['seen_version'] = version  # shown: not again at the next start, whatever closes it

    def close(self):
        self.frame.destroy()
        self.app.news_panel = None
        self.app.collect()  # the seen version saved now, not only when the launcher closes
        self.app.refresh_status()
