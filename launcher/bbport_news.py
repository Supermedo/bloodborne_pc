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
