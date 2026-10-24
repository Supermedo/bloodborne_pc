# SPDX-License-Identifier: GPL-2.0-or-later
"""The System page and the guided setup of the Windows launcher.

System & performance: the hardware check of bbport.de/install for this PC
(bbport_hardware), what to expect at 1080p, 1440p and 4K, and the requirements. The guided
setup walks a new player through the same check, the game folder and a graphics choice made
for their PC; every page of the launcher stays as it was for changing anything afterwards.

The launcher module binds itself (bind()) so this file uses its helpers, texts and colours.
"""
import os
from pathlib import Path
import re
import time
import webbrowser

from PIL import ImageTk

import bbport_benchmark as bench
import bbport_character as character
import bbport_display as display
import bbport_hardware as hardware
import bbport_saves as saves
import bbport_theme as theme

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


STATE_COLORS = {'ok': theme.GOLD_LIGHT, 'warn': theme.AMBER, 'bad': theme.BLOOD_LIGHT}
STATE_ICONS = {'ok': 'ok', 'warn': 'warn', 'bad': 'error'}
PART_ICONS = {'gpu': theme.ICON['display'], 'cpu': theme.ICON['cpu'], 'ram': theme.ICON['ram']}
PART_TITLES = {'gpu': 'Graphics card', 'cpu': 'Processor', 'ram': 'Memory'}


def tier_color(tier):
    return (theme.BLOOD_LIGHT, theme.AMBER, theme.TEXT, theme.TEXT_STRONG, theme.GOLD_LIGHT, theme.GOLD_LIGHT)[tier]


def before_colon(text):
    """A text's lead-in up to its colon: ':' or the full-width '：' of Chinese and Japanese."""
    return re.split(r'\s*[:：]', text, maxsplit=1)[0].strip()


def after_colon(text):
    """A text without its lead-in ("Expected: 120+ FPS at 1440p" -> "120+ FPS at 1440p")."""
    return re.split(r'\s*[:：]\s*', text, maxsplit=1)[-1]


def level_color(level):
    return (theme.BLOOD_LIGHT, theme.BLOOD_LIGHT, theme.AMBER, theme.TEXT, theme.GOLD_LIGHT, theme.GOLD_LIGHT)[level]


class SetupMixin:
    """Hardware rating, the System page and the setup guide, mixed into the Launcher."""

    # ---- hardware ------------------------------------------------------------------------------
    def init_hardware(self):
        self.gpu_line, self.cpu, self.ram, self.rating, self.guide = '', None, 0, None, None
        self.hw_ready = False
        self.rating_views = []  # callables redrawing what shows the rating

    def hardware_ready(self):
        """Tk thread: the detection finished (detect_gpu)."""
        self.hw_ready = True
        self.update_rating(force=True)
        self.auto_settings()

    def detected_gpu(self):
        """(table entry or None, chosen by hand, the Vulkan name). A card chosen by hand counts
        only for the maker of the card Vulkan reports (another model, the right memory size): a
        GeForce chosen on a Radeon PC (a stray click, a wheel turn over the list in 1.9.2's
        redesign) would recommend DLSS where it cannot run, so it is dropped."""
        name, _integrated, vram = hardware.parse_gpu_line(self.gpu_line) if self.gpu_line else ('', False, 0)
        found = hardware.match_gpu(name, vram) if name else None
        chosen = hardware.GPU_BY_ID.get(self.var('gpu_override', 'app').get())
        maker = hardware.vendor_of(name)
        if chosen and maker and chosen.vendor != maker:
            self.var('gpu_override', 'app').set('')
            chosen = None
        if chosen:
            return chosen, True, name
        return found, False, name

    def update_rating(self, force=False):
        """Rates the PC again and redraws what shows it, when anything it depends on changed."""
        key = (self.hw_ready, self.gpu_line, self.var('gpu_override', 'app').get(),
               self.cpu and self.cpu.name, self.ram)
        if not force and key == getattr(self, 'rating_key', None):
            return
        self.rating_key = key
        gpu = self.detected_gpu()[0]
        self.rating = hardware.assess(gpu, self.cpu, self.ram, tr=_) if gpu and self.cpu else None
        for view in list(self.rating_views):
            try:
                view()
            except self.tk.TclError:
                self.rating_views.remove(view)  # its widgets are gone

    def display_size(self):
        """The size of the monitor the game opens on (Picture & performance: Monitor)."""
        return self.game_monitor_size()

    def apply_settings(self, settings):
        for (store, key), value in settings.items():
            self.var(key, store).set(value)
        self.refresh_status()

    def expectation(self):
        """'60–120 FPS at 2560 × 1440' for the chosen output, with the rating's colour; or None."""
        if not self.rating:
            return None
        output = self.var('output_res', 'ini').get()
        level = self.rating.levels[hardware.resolution_class(output)]
        return (_('Expected: {} at {}').format(_(hardware.LEVELS[level]), output.replace('x', ' × ')),
                level_color(level))

    def show_rating_check(self):
        """The Play page: the verdict in the ready check, the expected frame rate under the quick settings."""
        icon, label = self.checks['system']
        r = self.rating
        if r:
            state = 'bad' if r.tier == 0 else 'warn' if r.tier == 1 else 'ok'
            icon.configure(text=theme.ICON['system'], fg=tier_color(r.tier))
            label.configure(text=_('Performance: {}').format(r.summary),
                            foreground=theme.TEXT if state == 'ok' else STATE_COLORS[state])
        else:
            gpu = self.detected_gpu()[0]
            icon.configure(text=theme.ICON['system'], fg=theme.MUTED)
            label.configure(text=_('Performance: pick your graphics card (System & tests)').replace(
                                _('System & tests'), _(*dict(L.NAV_EXPERT)['system']))
                            if self.hw_ready and not gpu else _('Performance: checking your PC…'),
                            foreground=theme.MUTED)
        expected = self.expectation()
        self.expect_label.configure(text=expected[0] + '   ›' if expected else '',
                                    fg=expected[1] if expected else theme.MUTED)
        self.settings_check()

    # ---- automatic settings --------------------------------------------------------------------
    def setting_names(self):
        """{key: (title, options or kind)} of the settings the profile sets, for showing them."""
        flags = {key: title for key, title, _on in L.EFFECTS}
        return {'output_res': (_('Output resolution'), L.OUTPUTS), 'upscaler': (_('Upscaler', 'Апскейлер'), L.UPSCALERS),
                'preset': (_('Quality preset'), L.PRESETS), 'fps_mode': (_('Frame rate', 'Частота кадров'), L.FPS_MODES),
                'frame_cap': (_('Frame cap (unlocked mode)'), L.FRAME_CAPS),
                'effect_game_aa': (_(*flags['effect_game_aa']), 'flag'),
                'effect_ssao': (_(*flags['effect_ssao']), 'flag'),
                'effect_dynamic_shadows': (_(*flags['effect_dynamic_shadows']), 'flag'),
                'effect_ssr': (_(*flags['effect_ssr']), 'flag'),
                'model_lod': (_('Model detail (LOD)'), L.LODS), 'object_motion': (_('Object motion vectors'), 'flag'),
                'sharpen': (_('Sharpening (RCAS)'), 'flag'), 'sharpness': (_('Sharpness'), 'number'),
                'frame_gen': (_('Frame generation'), L.FRAME_GENERATION)}

    def show_value(self, key, value):
        _title, kind = self.setting_names()[key]
        if kind == 'flag':
            return _('On', 'Вкл.') if value else _('Off', 'Выключен')
        if kind == 'number':
            return f'{float(value):.2f}'
        label = dict(kind).get(str(value))
        return _(*label).replace(L.WARN, '') if label else str(value)

    def current_settings(self):
        """{key: value} of what the profile covers, as set now."""
        out = {}
        for key in self.setting_names():
            store = 'app' if key in ('fps_mode', 'frame_cap') else 'ini'
            try:
                out[key] = self.var(key, store).get()
            except self.tk.TclError:
                out[key] = None
        return out

    def recommended(self, res=None):
        """(resolution class, output, [(store, key, value, why)]) for this PC, or None before the rating."""
        if not self.rating:
            return None
        available = hardware.outputs(*self.display_size())
        res = res if res in available else hardware.recommended_class(self.rating, available)
        output = available.get(res, '1920x1080')
        return res, output, hardware.profile(self.rating, res, output, self.current_settings(),
                                             self.var('goal', 'app').get())

    def differences(self):
        """[(key, current, recommended, why)] where the settings differ from the recommendation."""
        chosen = self.recommended()
        if not chosen:
            return []
        now = self.current_settings()
        same = lambda a, b: (abs(float(a) - float(b)) < 0.005 if isinstance(b, float) else
                             bool(a) == b if isinstance(b, bool) else str(a) == str(b))
        return [(key, now.get(key), value, why) for _store, key, value, why in chosen[2]
                if not same(now.get(key), value)]

    def apply_profile(self, entries, remember_undo=True):
        if remember_undo:
            now = self.current_settings()
            self.app['auto_undo'] = {key: now.get(key) for _s, key, _v, _w in entries}
        self.apply_settings({(store, key): value for store, key, value, _why in entries})
        self.app['auto_hw'] = hardware.signature(self.gpu_line, self.cpu, self.ram, self.display_size())
        for view in list(self.rating_views):
            try:
                view()
            except self.tk.TclError:
                self.rating_views.remove(view)

    def undo_profile(self):
        undo = self.app.get('auto_undo') or {}
        for key, value in undo.items():
            store = 'app' if key in ('fps_mode', 'frame_cap') else 'ini'
            if value is not None:
                self.var(key, store).set(value)
        self.app['auto_undo'] = {}
        self.refresh_status()
        for view in list(self.rating_views):
            try:
                view()
            except self.tk.TclError:
                self.rating_views.remove(view)

    def auto_settings(self):
        """At start, once the PC is rated: new hardware (or display) with automatic settings on →
        the settings for it, with an Undo on System & performance. The first time (an installation
        set up before this existed, or the guide skipped) only remembers the hardware. A new
        bbport.ini (a new folder) always gets them: its values are only the defaults, FSR 4 and every
        effect, which freeze Intel graphics and slow small cards down."""
        if not self.rating or not self.var('setup_done', 'app').get():
            return
        now = hardware.signature(self.gpu_line, self.cpu, self.ram, self.display_size())
        before = self.app.get('auto_hw', '')
        new_ini, self.ini_new = getattr(self, 'ini_new', False), False
        if not before and not new_ini:
            self.app['auto_hw'] = now
            return
        if new_ini or (now != before and self.var('auto_graphics', 'app').get()):
            chosen = self.recommended()
            if chosen:
                self.apply_profile(chosen[2])
                self.auto_note = _('New installation: the graphics settings were made for this PC.') if new_ini else \
                    _('New hardware detected: the graphics settings were adjusted to it.')
                self.status.configure(text=self.auto_note, fg=theme.GOLD_LIGHT)

    def profile_card(self, parent):
        """System & performance: the settings for this PC next to the current ones, Apply, Undo and
        the switch for doing it automatically."""
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)

        drawn = {'key': None}

        def draw(force=False):
            """Rebuilt only when the rating, the settings or the Undo changed (it has many rows)."""
            key = (getattr(self, 'rating_key', None), tuple(sorted(self.current_settings().items())),
                   bool(self.app.get('auto_undo')), self.var('auto_graphics', 'app').get())
            if key == drawn['key'] and not force:
                return
            drawn['key'] = key
            for child in body.winfo_children():
                child.destroy()
            chosen = self.recommended()
            if not chosen:
                tk.Label(body, text=_('Checking your PC…'), bg=theme.CARD, fg=theme.MUTED,
                         font=(theme.BODY, 10)).pack(anchor='w')
                return
            res, output, entries = chosen
            diff = {key for key, *_rest in self.differences()}
            head = tk.Frame(body, bg=theme.CARD)
            head.pack(fill='x')
            tk.Label(head, text=_('Settings for your PC'), bg=theme.CARD, fg=theme.GOLD_LIGHT,
                     font=(theme.HEAD, 12)).pack(side='left')
            state = (_('All settings match the recommendation.') if not diff else
                     _('1 setting differs from the recommendation.') if len(diff) == 1 else
                     _('{} settings differ from the recommendation.').format(len(diff)))
            tk.Label(head, text=state, bg=theme.CARD, fg=theme.GOLD_LIGHT if not diff else theme.AMBER,
                     font=(theme.BODY_MEDIUM, 9)).pack(side='left', padx=(px(14), 0), pady=(px(3), 0))
            tk.Label(body, text=_('Derived from the requirements and the hardware check on bbport.de for your PC at '
                                  '{} ({}). Chromatic aberration, depth of field, motion blur and sharpening are '
                                  'taste and stay as you set them.').format(output.replace('x', ' × '),
                                                                  _(hardware.LEVELS[self.rating.levels[res]])),
                     bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                     wraplength=px(700)).pack(anchor='w', pady=(px(4), px(8)))
            grid = tk.Frame(body, bg=theme.CARD)
            grid.pack(fill='x')
            grid.columnconfigure((0, 1, 2), weight=1, uniform='profile')
            for col, text in enumerate((_('Setting'), _('Now'), _('For your PC'))):
                tk.Label(grid, text=text.upper(), bg=theme.CARD, fg=theme.GOLD, font=(theme.HEAD, 8), anchor='w').grid(
                    row=0, column=col, sticky='w', padx=(0, px(14)), pady=(0, px(4)))
            now = self.current_settings()
            for index, (_store, key, value, why) in enumerate(entries):
                row = 1 + index * 3
                differs = key in diff
                tk.Frame(grid, bg=theme.BORDER, height=1).grid(row=row, column=0, columnspan=3, sticky='we',
                                                                pady=(px(4), px(5)))
                title = self.setting_names()[key][0]
                tk.Label(grid, text=title, bg=theme.CARD, fg=theme.TEXT_STRONG if differs else theme.TEXT,
                         font=(theme.BODY_SEMI if differs else theme.BODY, 9), anchor='w').grid(
                    row=row + 1, column=0, sticky='w', padx=(0, px(14)))
                tk.Label(grid, text=self.show_value(key, now.get(key)), bg=theme.CARD,
                         fg=theme.AMBER if differs else theme.MUTED, font=(theme.BODY, 9), anchor='w', justify='left',
                         wraplength=px(230)).grid(row=row + 1, column=1, sticky='w', padx=(0, px(14)))
                tk.Label(grid, text=('›  ' if differs else '') + self.show_value(key, value), bg=theme.CARD,
                         fg=theme.TEXT_STRONG if differs else theme.TEXT, font=(theme.BODY_SEMI if differs else theme.BODY, 9),
                         anchor='w', justify='left', wraplength=px(230)).grid(row=row + 1, column=2, sticky='w')
                tk.Label(grid, text=_(why), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 8), anchor='w',
                         justify='left', wraplength=px(680)).grid(row=row + 2, column=0, columnspan=3, sticky='w',
                                                                  pady=(px(1), 0))
            buttons = tk.Frame(body, bg=theme.CARD)
            buttons.pack(fill='x', pady=(px(12), 0))
            apply = ttk.Button(buttons, text=_('Apply the settings for my PC'), style='Card.Accent.TButton',
                               command=lambda: (self.apply_profile(entries), draw()))
            apply.pack(side='left')
            if not diff:
                apply.state(['disabled'])
            if self.app.get('auto_undo'):
                ttk.Button(buttons, text=_('Undo'), style='Card.TButton',
                           command=lambda: (self.undo_profile(), draw())).pack(side='left', padx=(px(6), 0))
            ttk.Checkbutton(body, text=_('Adjust them automatically when the hardware changes'),
                            variable=self.var('auto_graphics', 'app'), style='Card.TCheckbutton').pack(
                anchor='w', pady=(px(10), 0))
        draw()
        self.rating_views.append(draw)
        card.redraw = draw
        return card

    def settings_check(self):
        """The Play page's line about the settings (in the ready check)."""
        icon, label = self.checks['settings']
        if not self.rating:
            icon.configure(text='', fg=theme.MUTED)
            label.configure(text='', foreground=theme.MUTED)
            return
        diff = self.differences()
        if not diff:
            icon.configure(text=theme.ICON['ok'], fg=theme.GOLD_LIGHT)
            label.configure(text=_('Settings: adjusted to your new hardware (Undo on Picture & performance)')
                            if getattr(self, 'auto_note', None) else _('Settings: made for your PC and your goal'),
                            foreground=theme.TEXT)
        else:
            icon.configure(text=theme.ICON['info'], fg=theme.AMBER)
            label.configure(text=(_('Settings: 1 differs from the recommendation for your PC')
                                  if len(diff) == 1 else
                                  _('Settings: {} differ from the recommendation for your PC').format(len(diff))),
                            foreground=theme.AMBER)
        self.check_targets['settings'] = lambda: self.show('picture')
        self.check_targets['system'] = lambda: self.show('picture')
        for key in ('settings', 'system'):
            line, go = self.check_lines[key]
            go.pack(side='right', anchor='n')
            for widget in (line, go) + self.checks[key]:
                widget.configure(cursor='hand2')

    # ---- game tests (bbport_benchmark, bbport_report) ---------------------------------------------
    TEST_MODES = [('performance', ('Best settings for a frame rate goal',)),
                  ('compare', ('Compare one setting: pictures and frame rate',)),
                  ('stability', ('Stability test (for beta builds)',))]
    GOALS = [('60', ('60 FPS: the best picture that keeps 60',)), ('90', ('90 FPS',)),
             ('120', ('120 FPS: as smooth as the game allows',))]
    DURATIONS = [('10', ('10 minutes',)), ('30', ('30 minutes',)), ('60', ('1 hour',)), ('120', ('2 hours',))]
    TESTED = ('upscaler', 'preset', 'object_motion', 'effect_ssao', 'effect_dynamic_shadows', 'model_lod')
    COMPARABLE = ('preset', 'upscaler', 'output_res', 'model_lod', 'effect_ssao', 'effect_dynamic_shadows',
                  'object_motion', 'effect_motion_blur', 'effect_dof', 'effect_chromatic_aberration', 'effect_ssr',
                  'effect_game_aa', 'sharpen')

    def tests_dir(self):
        return L.CONFIG_DIR / 'tests'

    def test_running(self):
        return bool(getattr(self, 'bench', None)) and not getattr(self.bench, 'finished', False)

    def test_scenarios(self):
        return bench.scenarios(self.tests_dir() / 'scenarios')

    def compare_options(self, key):
        """[(value, text)] a setting can be compared over."""
        if key == 'preset':
            return [(v, _(*t)) for v, t in L.PRESETS]
        if key == 'upscaler':
            vendor = hardware.vendor_of(self.gpu_line or '')
            return [(v, _(*t)) for v, t in L.UPSCALERS if v in ('fsr4', 'fsr3', 'taa', 'off') or
                    (v == 'dlss' and vendor == 'nvidia')]
        if key == 'model_lod':
            return [(v, _(*t)) for v, t in L.LODS]
        if key == 'output_res':
            w, h = self.display_size()
            own = f'{w}x{h}'
            sizes = [own] + ([f'{w * 3 // 2}x{h * 3 // 2}'] if h < 1440 else []) + (
                ['3840x2160'] if h < 2160 and w * 9 == h * 16 else [])
            return [(s, s.replace('x', ' × ') + ('' if s == own else ' ' + _('(downsampled)'))) for s in sizes]
        return [(True, _('On', 'Вкл.')), (False, _('Off', 'Выключен'))]

    def compare_title(self, key):
        names = self.setting_names()
        flags = {k: t for k, t, _on in L.EFFECTS}
        return names[key][0] if key in names else _(*flags[key]) if key in flags else key

    def benchmark_card(self, parent):
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)
        self.bench, self.bench_rows, self.bench_folder = None, [], None
        tk.Label(body, text=_('Game tests'), bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.HEAD, 12)).pack(anchor='w')
        tk.Label(body, text=_('The launcher starts the game from your last save, the same way every time, and measures '
                              'everything while it runs: frame rate, frame times, stutter, memory, processor and '
                              'graphics card load. It plays on a copy of your save in a folder of its own: your saves '
                              'and settings stay as they are. The report has every chart, the pictures and the logs, '
                              'to keep or to share.'),
                 bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left', wraplength=px(720)).pack(
            anchor='w', pady=(px(4), px(10)))
        form = tk.Frame(body, bg=theme.CARD)
        form.pack(fill='x')
        form.columnconfigure(0, minsize=px(100))
        form.columnconfigure(1, weight=1)

        def line(row, title, widget):
            tk.Label(form, text=title, bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10)).grid(
                row=row, column=0, sticky='w', pady=px(4), padx=(0, px(14)))
            widget.grid(row=row, column=1, sticky='w', pady=px(4))
        line(0, _('Test'), self.choice(form, 'test_mode', 'app', self.TEST_MODES, 44, style='Card.TCombobox'))
        found = self.test_scenarios()
        self.test_scenario_list = found
        options = [(s.id, (s.name,)) for s in found] or [('hunters-view', ("Hunter's view",))]
        line(1, _('Scenario'), self.choice(form, 'test_scenario', 'app', options, 44, style='Card.TCombobox'))
        self.test_about = tk.Label(form, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                                   wraplength=px(560))
        self.test_about.grid(row=2, column=1, sticky='w')
        self.test_options = tk.Frame(body, bg=theme.CARD)
        self.test_options.pack(fill='x', pady=(px(6), 0))
        buttons = tk.Frame(body, bg=theme.CARD)
        buttons.pack(fill='x', pady=(px(10), 0))
        self.bench_start = ttk.Button(buttons, text=_('Start the test'), style='Card.Accent.TButton',
                                      command=self.start_benchmark)
        self.bench_start.pack(side='left')
        self.bench_cancel = ttk.Button(buttons, text=_('Cancel'), style='Card.TButton', command=self.cancel_benchmark)
        ttk.Button(buttons, text=_('Earlier tests'), style='Card.TButton',
                   command=lambda: os.startfile(self.tests_dir()) if self.tests_dir().is_dir() else None).pack(
            side='right')
        self.bench_list = tk.Frame(body, bg=theme.CARD)
        self.bench_list.pack(fill='x', pady=(px(10), 0))
        self.bench_note = tk.Label(body, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                                   wraplength=px(720))
        self.bench_note.pack(anchor='w', pady=(px(6), 0))
        self.bench_apply = tk.Frame(body, bg=theme.CARD)
        self.bench_apply.pack(fill='x')
        self.bench_views = getattr(self, 'bench_views', {})
        self.bench_views['system'] = {'start': self.bench_start, 'cancel': self.bench_cancel, 'list': self.bench_list,
                                      'note': self.bench_note, 'apply': self.bench_apply}
        for key in ('test_mode', 'test_scenario', 'test_compare'):
            self.var(key, 'app').trace_add('write', lambda *_a: self.root.after_idle(self.test_form))
        self.test_form()
        return card

    def scenario(self):
        chosen = self.var('test_scenario', 'app').get()
        return next((s for s in self.test_scenario_list if s.id == chosen), None) or (
            self.test_scenario_list[0] if self.test_scenario_list else None)

    def test_form(self):
        """The options of the chosen kind of test."""
        tk, px = self.tk, self.px
        if getattr(self, 'test_form_busy', False):
            return
        self.test_form_busy = True
        try:
            for child in self.test_options.winfo_children():
                child.destroy()
            mode, scenario = self.var('test_mode', 'app').get(), self.scenario()
            about = _(scenario.description) if scenario else ''
            if scenario and mode not in scenario.kinds:
                about += ' ' + _('(This scenario is not made for this kind of test.)')
            self.test_about.configure(text=about)
            form = tk.Frame(self.test_options, bg=theme.CARD)
            form.pack(fill='x')
            form.columnconfigure(0, minsize=px(100))
            title = lambda text, row: tk.Label(form, text=text, bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10)).grid(
                row=row, column=0, sticky='nw', pady=px(4), padx=(0, px(14)))
            if scenario and scenario.save == 'ask':
                title(_('Save'), 0)
                chosen = (self.app.get('test_saves') or {}).get(scenario.id, '')
                row = tk.Frame(form, bg=theme.CARD)
                row.grid(row=0, column=1, sticky='w', pady=px(4))
                source = scenario.save_from
                missing = (_('Needs the boss saves of the mod “{}” (Nexus Mods, free with an account): download '
                             '“{}”, unpack it, then choose its folder.').format(source.get('name', ''),
                                                                                source.get('file', ''))
                           if source else _('choose the save to start from (a folder with SPRJ0005 or its files)'))
                tk.Label(row, text=chosen or missing, bg=theme.CARD, fg=theme.TEXT if chosen else theme.AMBER,
                         font=(theme.BODY, 9), wraplength=px(400), justify='left').pack(side='left')
                self.ttk.Button(row, text=_('Choose…'), style='Card.TButton',
                                command=lambda: self.choose_test_save(scenario)).pack(side='left', padx=(px(10), 0))
            if mode == 'performance':
                title(_('Goal'), 1)
                self.choice(form, 'bench_goal', 'app', self.GOALS, 40, style='Card.TCombobox').grid(
                    row=1, column=1, sticky='w', pady=px(4))
            elif mode == 'stability':
                title(_('Duration'), 1)
                self.choice(form, 'test_minutes', 'app', self.DURATIONS, 20, style='Card.TCombobox').grid(
                    row=1, column=1, sticky='w', pady=px(4))
                tk.Label(form, text=_('With your own settings. A crash, a freeze or memory that keeps growing ends up '
                                      'in the report with the log lines around it.'), bg=theme.CARD, fg=theme.MUTED,
                         font=(theme.BODY, 9), wraplength=px(560), justify='left').grid(row=2, column=1, sticky='w')
            else:
                title(_('Setting'), 1)
                keys = [(k, (self.compare_title(k),)) for k in self.COMPARABLE]
                self.choice(form, 'test_compare', 'app', keys, 40, style='Card.TCombobox').grid(
                    row=1, column=1, sticky='w', pady=px(4))
                title(_('Values'), 2)
                values = tk.Frame(form, bg=theme.CARD)
                values.grid(row=2, column=1, sticky='w', pady=px(4))
                key = self.var('test_compare', 'app').get()
                current = self.current_settings().get(key, self.var(key, 'ini').get() if key in L.INI_DEFAULTS else None)
                self.compare_values = []
                for value, text in self.compare_options(key):
                    var = tk.BooleanVar(value=True)
                    self.compare_values.append((value, text, var))
                    mark = '  ·  ' + _('now') if str(value) == str(current) or (
                        isinstance(value, bool) and bool(current) == value and current not in (None, '')) else ''
                    self.ttk.Checkbutton(values, text=text + mark, variable=var, style='Card.TCheckbutton').pack(
                        anchor='w')
                title(_('Each'), 3)
                self.choice(form, 'test_repeats', 'app', [('1', ('once (about 1½ min per value)',)),
                                                          ('2', ('twice, interleaved (more exact)',))], 40,
                            style='Card.TCombobox').grid(row=3, column=1, sticky='w', pady=px(4))
        finally:
            self.test_form_busy = False

    def choose_test_save(self, scenario):
        folder = self.filedialog.askdirectory(title=_('The save to start from'))
        if not folder:
            return
        source = scenario.save_from
        if source:  # a save mod: this scenario's save, and those of the others from the same mod
            if not bench.mod_save(folder, source.get('folder', ''), source.get('file', '')):
                self.messagebox.showinfo('Bloodborne', _('The save for this test is not in that folder. Choose the '
                                                         'unpacked “{}” folder of the mod.').format(source.get('file', '')))
                return
            for other in self.test_scenario_list:
                if other.save_from and other.save_from.get('url') == source.get('url'):
                    found = bench.mod_save(folder, other.save_from.get('folder', ''), other.save_from.get('file', ''))
                    if found:
                        self.app.setdefault('test_saves', {})[other.id] = str(found)
            self.test_form()
            return
        found, _encrypted = saves.find_in(folder)
        if not found:
            self.messagebox.showinfo('Bloodborne', _('There is no save of this port, shadPS4 or bbhost in that folder '
                                                     '(SPRJ0005 with userdata0000 …).'))
            return
        self.app.setdefault('test_saves', {})[scenario.id] = str(found[0])
        self.test_form()

    def describe_change(self, start, settings):
        """'Quality preset: Native AA, …' for what a run changed (or 'the settings for your PC')."""
        parts = []
        for key, value in settings.items():
            if start.get(key) == value:
                continue
            try:
                options = dict((str(v), t) for v, t in self.compare_options(key)) if key in self.COMPARABLE else {}
                shown = options.get(str(value)) or self.show_value(key, value)
            except (KeyError, ValueError):
                shown = str(value)
            parts.append(f'{self.compare_title(key)}: {shown}')
        return ', '.join(parts) or _('the settings for your PC')

    def start_benchmark(self, view='system', mode=None):
        """A game test from System & tests (any kind), or the measurement for the player's goal
        from Picture & performance (view 'picture')."""
        import platform
        import time as clock
        if self.process or bench.game_pids():
            self.messagebox.showinfo('Bloodborne', _('Close the game first.'))
            return
        if self.test_running():
            self.messagebox.showinfo('Bloodborne', _('A game test is running already.'))
            return
        v = self.bench_views[view]
        self.bench_start, self.bench_cancel, self.bench_list, self.bench_note, self.bench_apply = (
            v['start'], v['cancel'], v['list'], v['note'], v['apply'])
        save = self.save_target()
        if not getattr(self, 'test_scenario_list', None):
            self.test_scenario_list = self.test_scenarios()
        scenario = (self.scenario() if view == 'system' else
                    next((s for s in self.test_scenario_list if s.id == 'hunters-view'), None) or self.scenario())
        mode = mode or self.var('test_mode', 'app').get()
        if not scenario:
            return
        # a scenario with a save of its own (Cathedral Ward) needs none of the player's
        own_save = scenario.save in ('own', 'ask')
        if not L.game_info(self.var('game_dir', 'app').get()) or own_save and not saves.is_save_folder(save):
            self.messagebox.showinfo('Bloodborne', _('The test needs your game folder and a save to start from: '
                                                     'play until the game has saved once.'))
            return
        chosen_save = (self.app.get('test_saves') or {}).get(scenario.id)
        if scenario.save == 'ask' and not chosen_save:
            self.messagebox.showinfo('Bloodborne', _('Choose the save this scenario starts from first.'))
            return
        if mode == 'stability' and not scenario.loop:
            self.messagebox.showinfo('Bloodborne', _('This scenario has no loop for a stability test.'))
            return
        if scenario.missing_routes():
            self.messagebox.showinfo('Bloodborne', _('The route of this test is not recorded yet: record it first.'))
            return
        self.collect()
        minutes = 0
        if mode == 'performance':
            chosen = self.recommended()
            if not chosen:
                return
            _res, output, entries = chosen
            start = {key: value for store, key, value, _why in entries if store == 'ini' and key in self.TESTED}
            start['output_res'] = output
            goal_fps = (int(self.var('bench_goal', 'app').get() or 60) if view == 'system' else
                        hardware.GOAL_FPS.get(self.var('goal', 'app').get(), 60))
            plan = bench.Plan(start, goal_fps)
            runs = _('up to 7 runs')
        elif mode == 'compare':
            key = self.var('test_compare', 'app').get()
            picked = [(text, {key: value}) for value, text, var in getattr(self, 'compare_values', []) if var.get()]
            if len(picked) < 2:
                self.messagebox.showinfo('Bloodborne', _('Choose at least two values to compare.'))
                return
            current = self.current_settings().get(key, self.var(key, 'ini').get() if key in L.INI_DEFAULTS else '')
            repeats = int(self.var('test_repeats', 'app').get() or 1)
            plan = bench.Variants({key: current}, picked, repeats)
            runs = _('{} runs').format(len(picked) * repeats)
        else:
            plan = bench.Soak({})
            minutes = int(self.var('test_minutes', 'app').get() or 10)
            runs = _('{} minutes').format(minutes)
        w, h = self.display_size()
        info = {'Launcher': f'v{L.VERSION}', 'Graphics card': self.gpu_line or '?',
                'Processor': self.cpu.name if self.cpu else '?', 'Memory': f'{self.ram} GB', 'Display': f'{w} × {h}',
                'Windows': platform.version(), 'Game': ' '.join(L.game_info(self.var('game_dir', 'app').get()) or ()),
                'Settings': self.diagnostics().split('Settings: ', 1)[-1].split('\n', 1)[0]}
        folder = self.tests_dir() / (clock.strftime('%Y-%m-%d_%H%M%S') + '_' + mode)
        settings = {**self.app, 'online': False, 'fullscreen': True}
        test = bench.Test(L.PORT_DIR, L.run_command(), L.game_environment(settings), self.save_user_dir(),
                          self.var('game_dir', 'app').get(),
                          L.ini_path().read_text(encoding='utf-8') if L.ini_path().is_file() else '',
                          scenario, plan, folder,
                          lambda kind, data: self.ui_calls.put(lambda: self.benchmark_event(kind, data)),
                          kind=mode, minutes=minutes, save=chosen_save, info=info)
        self.bench, self.bench_folder = test, folder
        test.goal_fps, test.view = getattr(plan, 'goal', 60), view
        for child in list(self.bench_list.winfo_children()) + list(self.bench_apply.winfo_children()):
            child.destroy()
        self.bench_rows = []
        self.bench_note.configure(text=_('Preparing ({}). The game opens in fullscreen; leave the PC alone until it is '
                                         'done.').format(runs) + ' ' +
                                  _('A key, the mouse or a controller used during the test ends it.'), fg=theme.MUTED)
        self.bench_start.pack_forget()
        self.bench_cancel.pack(side='left')
        self.play_button.configure(state='disabled')
        self.status.configure(text=_('Game test running: the game opens and closes by itself.'), fg=theme.GOLD_LIGHT)
        test.start(save)

    def cancel_benchmark(self):
        if self.bench:
            self.bench.cancelled = True
            self.bench_note.configure(text=_('Stopping: the game closes…'))

    def benchmark_event(self, kind, data):
        tk, px = self.tk, self.px
        test = self.bench
        if not test:
            return
        if kind == 'run':
            index, name = data
            line = tk.Frame(self.bench_list, bg=theme.CARD)
            line.pack(fill='x', pady=px(2))
            names = {'recommended': _("Your PC's recommendation"), 'stability': _('Stability run'),
                     'lowest': _('Lowest settings (what your PC can reach at all)')}
            text = tk.Label(line, text=names.get(name, name if test.kind == 'compare' else '…'), bg=theme.CARD,
                            fg=theme.TEXT, font=(theme.BODY, 9), anchor='w', justify='left', wraplength=px(500))
            text.pack(side='left', fill='x', expand=True)
            value = tk.Label(line, text=_('starting…'), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY_SEMI, 9))
            value.pack(side='right')
            self.bench_rows.append((name, text, value))
            self.bench_note.configure(text=_('Run {}: the game is starting.').format(index))
        elif kind == 'loaded' and self.bench_rows:
            self.bench_rows[-1][2].configure(text=_('measuring…'))
        elif kind == 'progress' and self.bench_rows:
            self.bench_rows[-1][2].configure(text=_('round {}, {} min left').format(data['round'], data['left']))
        elif kind == 'result':
            if not self.bench_rows:
                return
            name, text, value = self.bench_rows[-1]
            if name not in ('recommended', 'lowest', 'stability') and test.kind != 'compare':
                text.configure(text=self.describe_change(test.plan.start, data['settings']))
            if data['status'] != 'ok' or 'fps' not in data:
                value.configure(text={'crash': _('crashed'), 'freeze': _('froze'), 'no start': _('did not start'),
                                      'cancelled': _('cancelled'), 'input': _('stopped: your input')}.get(
                                          data['status'], _('failed')) +
                                (f" ({data.get('message')})" if data.get('message') else ''), fg=theme.BLOOD_LIGHT)
            else:
                keeps = isinstance(test.plan, bench.Plan) and test.plan.keeps(data)
                value.configure(text=f"{data['fps']:.0f} FPS" + (f"  ·  1 % {data['low1']:.0f}" if data.get('low1')
                                                                else ''),
                                fg=theme.GOLD_LIGHT if keeps else theme.TEXT)
        elif kind in ('done', 'error'):
            test.finished = True
            self.bench_cancel.pack_forget()
            self.bench_start.pack(side='left')
            self.status.configure(text='', fg=theme.MUTED)
            self.refresh_status()
            if kind == 'error':
                self.bench_note.configure(text=_('The test stopped: {}').format(data), fg=theme.BLOOD_LIGHT)
                self.result_buttons(self.bench_folder)
                return
            plan, results, folder = data
            self.benchmark_result(test, plan, results, folder)

    def result_buttons(self, folder):
        ttk, px = self.ttk, self.px
        if not folder or not Path(folder).is_dir():
            return
        report = Path(folder) / 'report.html'
        if report.is_file():
            ttk.Button(self.bench_apply, text=_('Open the report'), style='Card.Accent.TButton',
                       command=lambda: webbrowser.open(report.as_uri())).pack(side='left', pady=(px(8), 0))

        def export():
            import bbport_report
            try:
                path = bbport_report.export(folder)
                self.bench_note.configure(text=_('Saved: {}').format(path), fg=theme.GOLD_LIGHT)
            except OSError as error:
                self.bench_note.configure(text=_('Could not save it: {}').format(error), fg=theme.BLOOD_LIGHT)
        ttk.Button(self.bench_apply, text=_('Save as zip'), style='Card.TButton', command=export).pack(
            side='left', padx=(px(8), 0), pady=(px(8), 0))
        ttk.Button(self.bench_apply, text=_('Open the folder'), style='Card.TButton',
                   command=lambda: os.startfile(folder)).pack(side='left', padx=(px(8), 0), pady=(px(8), 0))

    def benchmark_result(self, test, plan, results, folder):
        ttk, px = self.ttk, self.px
        ok = [r for r in results if r['status'] == 'ok' and 'fps' in r]
        if any(r['status'] == 'input' for r in results):  # what was measured is not the scenario's any more
            self.bench_note.configure(text=_('The test stopped: a key, the mouse or a controller was used. Start it '
                                             'again and leave the PC alone until it is done.'), fg=theme.BLOOD_LIGHT)
            self.result_buttons(folder)
            return
        if test.kind == 'stability':
            r = results[0] if results else None
            if not r:
                note = _('The test did not run.')
            elif r['status'] == 'ok':
                note = _('No crash and no freeze in {:.0f} minutes ({:.0f} FPS on average).').format(
                    r.get('seconds', 0) / 60, r.get('fps', 0))
                if (r.get('ram_growth_gb') or 0) > 0.5:
                    note += ' ' + _('But the game\'s memory grew by {:.1f} GB: see the report.').format(r['ram_growth_gb'])
            else:
                note = _('The game {} after {:.0f} minutes: the report has the log lines and charts up to that '
                         'moment.').format({'crash': _('crashed'), 'freeze': _('froze')}.get(r['status'], r['status']),
                                           r.get('seconds', 0) / 60)
            self.bench_note.configure(text=note, fg=theme.TEXT if r and r['status'] == 'ok' else theme.BLOOD_LIGHT)
            self.result_buttons(folder)
            return
        if not ok:
            self.bench_note.configure(text=_('No run finished; the report has the logs.'), fg=theme.BLOOD_LIGHT)
            self.result_buttons(folder)
            return
        if test.kind == 'compare':
            best = max(ok, key=lambda r: r['fps'])
            self.bench_note.configure(text=_('Done. The report shows the pictures side by side with the frame rates; '
                                             'the fastest here: {} ({:.0f} FPS).').format(best['name'], best['fps']),
                                      fg=theme.TEXT)
            self.result_buttons(folder)
            return
        choice = plan.choice or max(ok, key=lambda r: r['fps'])
        goal = getattr(test, 'goal_fps', 60)
        self.app['last_measure'] = {'fps': choice['fps'], 'low1': choice.get('low1'), 'goal': goal,
                                    'date': time.strftime('%d.%m.%Y'), 'folder': str(folder)}
        if getattr(self, 'picture_view', None):
            self.picture_view()
        note = _('Best for your goal: {} with {:.0f} FPS.').format(
            self.describe_change(plan.start, choice['settings']), choice['fps'])
        limit = getattr(plan, 'floor', None)
        if limit and limit * 0.97 < goal:
            note += ' ' + _('Your PC reaches about {:.0f} FPS at most here: above that the processor or the game '
                            'itself sets the limit, not the graphics settings.').format(limit)
        if not plan.choice:
            note += ' ' + _('No setting kept the goal; this one came closest.')
        self.bench_note.configure(text=note, fg=theme.TEXT)
        names = self.setting_names()
        entries = [('ini', key, value, 'performance test') for key, value in choice['settings'].items() if key in names]
        ttk.Button(self.bench_apply, text=_('Apply'), style='Card.Accent.TButton',
                   command=lambda: (self.apply_profile(entries), self.bench_note.configure(
                       text=note + ' ' + _('Applied (Undo on this page).')))).pack(side='left', pady=(px(8), 0),
                                                                                   padx=(0, px(8)))
        self.result_buttons(folder)

    # ---- saves from other versions -------------------------------------------------------------
    def save_user_dir(self):
        return Path(self.var('user_dir', 'app').get() or L.DATA_DIR / 'user')

    def save_target(self):
        return saves.target_dir(self.save_user_dir(), self.var('game_dir', 'app').get())

    def find_saves(self, on_done):
        """Searches this PC for saves in a helper thread (it walks folders), then calls on_done(sets)
        in the Tk thread; one search at a time, its result kept for the next card."""
        import threading
        self.save_waiters = getattr(self, 'save_waiters', []) + [on_done]
        if getattr(self, 'save_searching', False):
            return
        self.save_searching = True
        port, game, mine = L.PORT_DIR, self.var('game_dir', 'app').get(), self.save_target()

        def work():
            try:
                found = saves.scan(port, game, exclude=[mine])
            except Exception:  # a search must never take the launcher down
                found = []

            def done():
                self.save_searching, self.save_found = False, found
                waiters, self.save_waiters = self.save_waiters, []
                for waiter in waiters:
                    waiter(found)
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def describe_save(self, save):
        count = len(save.slots)
        if not count:
            return _('no characters')
        names = ', '.join(save.names)
        text = _('1 character') if count == 1 else _('{} characters').format(count)
        return f'{text}: {names}' if names else text

    def saves_card(self, parent, limit=None, title=None, hide_empty=False, manager=False):
        """The saves found from other versions, each with Import; Search again, Choose a folder.
        With hide_empty the card stays hidden until something is found; with manager (the Saves
        page) without this port's own line and the Backups button (the cards above have them)."""
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)
        state = {'sets': None, 'chosen': False}

        def draw():
            if not body.winfo_exists():
                return
            for child in body.winfo_children():
                child.destroy()
            if title:
                tk.Label(body, text=title, bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.HEAD, 12)).pack(
                    anchor='w', pady=(0, px(6)))
            here = self.save_target()
            mine = saves.SaveSet('', here) if saves.is_save_folder(here) else None
            if not manager:
                self.state_line(body, 'ok' if mine and mine.slots else 'info',
                                _('This port: {}, last played {}').format(self.describe_save(mine), mine.when())
                                if mine else _('This port has no saves yet.'), theme.CARD, wrap=640).pack(fill='x')
            sets = state['sets']
            top = 0 if manager else px(10)
            if sets is None:
                tk.Label(body, text=_('Searching this PC for saves of shadPS4, Awaken\'s port and bbhost…'),
                         bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9)).pack(anchor='w', pady=(top, 0))
            elif not sets:
                tk.Label(body, text=_('No saves from shadPS4, Awaken\'s port or bbhost found on this PC. If yours '
                                      'are somewhere else (a backup, a PS4 export), choose their folder.'),
                         bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                         wraplength=px(640)).pack(anchor='w', pady=(top, 0))
            for save in (sets or [])[:limit or None]:
                tk.Frame(body, bg=theme.BORDER, height=1).pack(fill='x', pady=(px(10), px(8)))
                line = tk.Frame(body, bg=theme.CARD)
                line.pack(fill='x')
                ttk.Button(line, text=_('Import'), style='Card.TButton',
                           command=lambda s=save: self.import_save(s, search)).pack(side='right', anchor='center')
                words = tk.Frame(line, bg=theme.CARD)
                words.pack(side='left', fill='x', expand=True)
                tk.Label(words, text=save.source, bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_SEMI, 10),
                         anchor='w', justify='left', wraplength=px(520)).pack(anchor='w')
                tk.Label(words, text=self.describe_save(save) + '  ·  ' + _('last played {}').format(save.when()),
                         bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 9), anchor='w', justify='left',
                         wraplength=px(520)).pack(anchor='w', pady=(px(1), 0))
                tk.Label(words, text=str(save.folder), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 8),
                         anchor='w', justify='left', wraplength=px(520)).pack(anchor='w', pady=(px(2), 0))
            buttons = tk.Frame(body, bg=theme.CARD)
            buttons.pack(anchor='w', pady=(px(12), 0))
            ttk.Button(buttons, text=_('Search again'), style='Card.TButton', command=search).pack(side='left')
            ttk.Button(buttons, text=_('Choose a folder…'), style='Card.TButton', command=choose).pack(
                side='left', padx=(px(6), 0))
            backups = self.save_user_dir() / 'save_backups'
            if backups.is_dir() and not manager:
                ttk.Button(buttons, text=_('Backups'), style='Card.TButton',
                           command=lambda: self.open_path(backups)).pack(side='left', padx=(px(6), 0))

        def show(found):
            if not card.winfo_exists():
                return
            state['sets'] = found
            if hide_empty and not found:
                card.pack_forget()
                return
            draw()
            if hide_empty and not card.winfo_ismapped():
                card.pack(**card.pack_options)

        def search():
            state['sets'] = None
            draw()
            self.find_saves(show)

        def choose():
            chosen = self.choose_save_folder()
            if chosen:
                state['sets'] = chosen
                draw()
                if hide_empty and not card.winfo_ismapped():
                    card.pack(**card.pack_options)
        def start():  # after the caller placed the card (and gave hide_empty its pack_options)
            if getattr(self, 'save_found', None) is not None and not getattr(self, 'save_searching', False):
                show(self.save_found)
            else:
                if not hide_empty:
                    draw()
                self.find_saves(show)
        self.root.after_idle(start)
        card.redraw = draw
        return card

    def choose_save_folder(self):
        """[SaveSet] in a folder the player picks (a backup, a PS4 export), or None."""
        chosen = self.filedialog.askdirectory(title=_('Choose the folder with the saves'),
                                              initialdir=str(Path.home()))
        if not chosen:
            return None
        found, encrypted = saves.find_in(chosen)
        found = [s for s in (saves.SaveSet(saves.label_for(f), f) for f in found)]
        usable = [s for s in found if s.complete]
        if not usable:
            text = (_('These are a PS4\'s own encrypted backups (sdimg_…); they cannot be used. Export the save with '
                      'Apollo Save Tool on the PS4 instead.') if encrypted and not found else
                    _('No complete Bloodborne save in {} or its subfolders: it needs userdata0010 and at least one '
                      'character (userdata0000 to userdata0009).').format(chosen))
            self.messagebox.showerror('Bloodborne', text)
            return None
        return sorted(usable, key=lambda s: -s.modified)

    def import_save(self, save, refresh):
        if self.process:
            self.messagebox.showinfo('Bloodborne', _('Close the game first; it writes its saves while it runs.'))
            return
        user, here = self.save_user_dir(), self.save_target()
        mine = saves.SaveSet('', here) if saves.is_save_folder(here) else None
        question = _('Import the saves from {} ({}, last played {})?').format(
            save.source, self.describe_save(save), save.when())
        if mine:
            question += '\n\n' + _('They replace this port\'s saves ({}). Those are packed into {} first, so '
                                   'nothing is lost.').format(self.describe_save(mine), user / 'save_backups')
        if not self.messagebox.askyesno('Bloodborne', question):
            return
        try:
            archive = saves.import_set(save, user, self.var('game_dir', 'app').get())
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('Could not import the saves: {}').format(error))
            return
        done = _('Saves imported. Your characters are in the game\'s Load Game menu.')
        if archive:
            done += '\n\n' + _('Your previous saves: {}').format(archive)
        self.messagebox.showinfo('Bloodborne', done)
        self.save_found = None
        self.refresh_status()
        self.refresh_saves()
        refresh()

    # ---- the save manager (Saves page) -----------------------------------------------------------
    def game_running(self):
        """The game runs (from here, a shortcut or Steam): it writes its saves while it runs."""
        return bool(self.process) or bool(bench.game_pids())

    def refresh_saves(self, only_if_changed=False):
        """The characters and the backups drawn anew (on a page change only when the files changed)."""
        made_from = (str(self.save_target()), L.folder_entries(self.save_target()),
                     L.folder_entries(saves.backup_dir(self.save_user_dir())), self.var('game_dir', 'app').get(),
                     self.var('language', 'app').get())
        if only_if_changed and made_from == getattr(self, 'saves_made_from', None):
            return
        self.saves_made_from = made_from
        for view in ('characters_view', 'backups_view'):
            card = getattr(self, view, None)
            if card is not None and card.winfo_exists():
                card.redraw()

    def characters_card(self, parent):
        """This port's characters: what each is now (level, attributes, weapons, the bosses it beat)
        and, folded beneath it, what it was in each backup."""
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)
        opened = {}  # slot: 'some' or 'all' of its earlier states shown

        def draw():
            if not body.winfo_exists():
                return
            for child in body.winfo_children():
                child.destroy()
            here = self.save_target()
            mine = saves.SaveSet('', here) if saves.is_save_folder(here) else None
            if not mine or not mine.slots:
                self.state_line(body, 'info', _('No characters yet. The game makes them, or bring yours from another '
                                                'version (below).'), theme.CARD, wrap=640).pack(fill='x')
            else:
                words = character.names(self.var('game_dir', 'app').get(), self.var('language', 'app').get())
                items = [b for b in saves.backups(self.save_user_dir()) if b.ok]
                for index, slot in enumerate(mine.slots):
                    if index:
                        tk.Frame(body, bg=theme.BORDER, height=1).pack(fill='x', pady=px(16))
                    self.character_view(body, here / slot, words, items, opened, draw)
                hint = _('In the game, {} loads the character you played last; {} on the title screen lists all '
                         'of them.').format(words.word(character.CONTINUE_TEXT), words.word(character.LOAD_GAME_TEXT))
                tk.Label(body, text=hint, bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                         wraplength=px(640)).pack(anchor='w', pady=(px(18), 0))
            buttons = tk.Frame(body, bg=theme.CARD)
            buttons.pack(anchor='w', pady=(px(14), 0))
            if mine and mine.slots:
                ttk.Button(buttons, text=_('Back up now'), style='Card.Accent.TButton', command=self.backup_now).pack(
                    side='left', padx=(0, px(6)))
            ttk.Button(buttons, text=_('Open the folder'), style='Card.TButton',
                       command=lambda: self.open_path(here if here.is_dir() else self.save_user_dir())).pack(side='left')
        draw()
        card.redraw = draw
        return card

    STATES_SHOWN = 3

    def character_view(self, body, path, words, items, opened, redraw):
        """One character: name, level, play time, when it was saved; its attributes; its weapons, the
        bosses it beat, its echoes, insight and deaths; then its states in the backups."""
        import datetime
        tk, px = self.tk, self.px
        info = character.read_file(path)
        name = info.name if info else saves.character_name(path)
        head = tk.Frame(body, bg=theme.CARD)
        head.pack(fill='x')
        tk.Label(head, text=name or _('A character without a readable name'), bg=theme.CARD,
                 fg=theme.TEXT_STRONG if name else theme.MUTED, font=(theme.BODY_SEMI, 13)).pack(side='left')
        if info:
            tk.Label(head, text=f'{words.word(character.LEVEL_TEXT)} {info.level}', bg=theme.CARD, fg=theme.GOLD_LIGHT,
                     font=(theme.BODY_SEMI, 13)).pack(side='right')
        try:
            when = datetime.datetime.fromtimestamp(path.stat().st_mtime).strftime('%d.%m.%Y %H:%M')
        except OSError:
            when = '?'
        facts = [_('Slot {}').format(int(path.name[-4:]) + 1), _('saved {}').format(when)]
        if info:
            hours, minutes = info.played // 3600, info.played % 3600 // 60
            facts.insert(1, f'{words.word(character.PLAY_TIME_TEXT)} {hours}:{minutes:02d}')
        tk.Label(body, text='  ·  '.join(facts), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9)).pack(anchor='w')
        if not info:
            tk.Label(body, text=_('Its level, weapons and bosses cannot be read from this save.'), bg=theme.CARD,
                     fg=theme.MUTED, font=(theme.BODY, 9)).pack(anchor='w', pady=(px(6), 0))
            return
        tiles = tk.Frame(body, bg=theme.CARD)
        tiles.pack(fill='x', pady=(px(12), 0))
        for column, (key, value) in enumerate(info.attributes.items()):
            tiles.columnconfigure(column, weight=1, uniform='attributes')
            tile = tk.Frame(tiles, bg=theme.SURFACE, padx=px(10), pady=px(6))
            tile.grid(row=0, column=column, sticky='we', padx=(px(6) if column else 0, 0))
            tk.Label(tile, text=words.attribute(key), bg=theme.SURFACE, fg=theme.MUTED, font=(theme.BODY, 8)).pack(
                anchor='w')
            tk.Label(tile, text=value, bg=theme.SURFACE, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 13)).pack(
                anchor='w')
        rows = tk.Frame(body, bg=theme.CARD)
        rows.pack(fill='x', pady=(px(10), 0))
        self.character_facts(rows, info, words, theme.CARD, 10)
        more = [f'{words.word(character.ECHOES_TEXT)} {info.echoes}',
                f'{words.word(character.INSIGHT_TEXT)} {info.insight}', _('Deaths') + f' {info.deaths}']
        if info.journey:
            more.append(f'NG+{info.journey}')
        tk.Label(body, text='  ·  '.join(more), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9)).pack(
            anchor='w', pady=(px(4), 0))
        self.earlier_states(body, path.name, info, words, items, opened, redraw)

    def character_facts(self, rows, info, words, bg, size):
        """Its weapons and the bosses it beat, as two labelled rows."""
        tk, px = self.tk, self.px
        weapons = [w for w in map(words.weapon, info.weapons) if w]
        bosses = [b for b in map(words.boss, info.bosses) if b]
        lines = ((_('Weapons'), '  ·  '.join(weapons) or '–'),
                 (_('Bosses') + f' {len(info.bosses)}/{len(character.BOSSES)}',
                  '  ·  '.join(bosses) or _('None beaten yet')))
        rows.columnconfigure(0, minsize=px(92))
        for row, (label, value) in enumerate(lines):
            tk.Label(rows, text=label, bg=bg, fg=theme.MUTED, font=(theme.BODY, 9)).grid(
                row=row, column=0, sticky='nw', padx=(0, px(14)), pady=px(1))
            tk.Label(rows, text=value, bg=bg, fg=theme.TEXT, font=(theme.BODY, size), justify='left',
                     wraplength=px(560)).grid(row=row, column=1, sticky='w', pady=px(1))

    def earlier_states(self, body, slot, now, words, items, opened, redraw):
        """The character as it was in each backup, newest first; what differs from now stands out."""
        tk, px = self.tk, self.px
        states = [(item, then) for item in items
                  for then in [saves.backup_characters(item).get(slot)] if then and then.name == now.name]
        if not states:
            return
        shown = opened.get(slot)
        toggle = tk.Label(body, text=(_('Hide the earlier states') if shown else
                                      _('Earlier states in your backups ({})').format(len(states))) + '  ›',
                          bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_MEDIUM, 9), cursor='hand2')
        toggle.pack(anchor='w', pady=(px(12), 0))
        toggle.bind('<Button-1>', lambda _e: (opened.update({slot: None if shown else 'some'}), redraw()))
        if not shown:
            return
        bg = theme.SURFACE
        well = tk.Frame(body, bg=bg, padx=px(16), pady=px(10))
        well.pack(fill='x', pady=(px(8), 0))
        for index, (item, then) in enumerate(states if shown == 'all' else states[:self.STATES_SHOWN]):
            if index:
                tk.Frame(well, bg=theme.BORDER, height=1).pack(fill='x', pady=(px(10), 0))
            line = tk.Frame(well, bg=bg)
            line.pack(fill='x', pady=(px(8) if index else 0, 0))
            restore = tk.Label(line, text=_('Restore'), bg=bg, fg=theme.MUTED, font=(theme.BODY_MEDIUM, 9),
                               cursor='hand2')
            restore.pack(side='right')
            restore.bind('<Button-1>', lambda _e, i=item, t=then: self.restore_character(i, slot, t, words))
            restore.bind('<Enter>', lambda _e, w=restore: w.configure(fg=theme.TEXT))
            restore.bind('<Leave>', lambda _e, w=restore: w.configure(fg=theme.MUTED))
            tk.Label(line, text=item.created.strftime('%d.%m.%Y %H:%M') + '  ·  ' + self.backup_title(item), bg=bg,
                     fg=theme.GOLD_LIGHT, font=(theme.BODY_SEMI, 9)).pack(side='left')
            if then.summary() == now.summary():
                tk.Label(line, text=_('same as now'), bg=bg, fg=theme.MUTED, font=(theme.BODY, 9)).pack(
                    side='left', padx=(px(10), 0))
            values = tk.Frame(well, bg=bg)
            values.pack(anchor='w', pady=(px(4), 0))
            pieces = [(words.word(character.LEVEL_TEXT), then.level, now.level)] + [
                (words.attribute(key), value, now.attributes[key]) for key, value in then.attributes.items()]
            for number, (word, value, current) in enumerate(pieces):
                changed = value != current
                tk.Label(values, text=f'{word} {value}', bg=bg, fg=theme.TEXT_STRONG if changed else theme.MUTED,
                         font=(theme.BODY_SEMI if changed else theme.BODY, 9)).pack(
                    side='left', padx=(px(12) if number else 0, 0))
            rows = tk.Frame(well, bg=bg)
            rows.pack(fill='x', pady=(px(2), 0))
            self.character_facts(rows, then, words, bg, 9)
        if len(states) > self.STATES_SHOWN:
            more = tk.Label(well, text=(_('Show fewer') if shown == 'all' else _('Show all ({})').format(len(states)))
                            + '  ›', bg=bg, fg=theme.GOLD_LIGHT, font=(theme.BODY_MEDIUM, 9), cursor='hand2')
            more.pack(anchor='w', pady=(px(10), 0))
            more.bind('<Button-1>', lambda _e: (opened.update({slot: 'some' if shown == 'all' else 'all'}), redraw()))

    BACKUPS_SHOWN = 6

    def backup_title(self, item):
        """What a backup is: the player's name for it, else why it was made (and from what)."""
        if item.label:
            return item.label
        words = _(saves.REASONS[item.reason])
        return f'{words}: {item.detail}' if item.detail else words

    def backups_card(self, parent):
        """The backups, newest first: what each is, when, who is in it; Restore, Rename, Copy to,
        Show in folder, Delete. Back up now, Add a backup file, the automatic backup switch."""
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)
        self.backups_state = state = {'all': False, 'rename': None}

        def draw():
            if not body.winfo_exists():
                return
            for child in body.winfo_children():
                child.destroy()
            ttk.Checkbutton(body, text=_('Back up automatically before every start'),
                            variable=self.var('auto_backup', 'app'), style='Card.TCheckbutton').pack(anchor='w')
            tk.Label(body, text=_('Only when you played since the last backup. The last {} automatic backups are '
                                  'kept; the ones you make yourself always.').format(saves.AUTO_KEEP),
                     bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left',
                     wraplength=px(640)).pack(anchor='w', padx=(px(26), 0))
            items = [b for b in saves.backups(self.save_user_dir()) if b.ok]
            if not items:
                tk.Label(body, text=_('No backups yet.'), bg=theme.CARD, fg=theme.MUTED,
                         font=(theme.BODY, 10)).pack(anchor='w', pady=(px(12), 0))
            for item in items if state['all'] else items[:self.BACKUPS_SHOWN]:
                self.backup_row(body, item, state, draw)
            if len(items) > self.BACKUPS_SHOWN:
                more = tk.Label(body, text=(_('Show fewer') if state['all'] else
                                            _('Show all ({})').format(len(items))) + '  ›',
                                bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_MEDIUM, 9), cursor='hand2')
                more.pack(anchor='w', pady=(px(10), 0))
                more.bind('<Button-1>', lambda _e: (state.update(all=not state['all']), draw()))
            buttons = tk.Frame(body, bg=theme.CARD)
            buttons.pack(anchor='w', pady=(px(14), 0))
            ttk.Button(buttons, text=_('Back up now'), style='Card.TButton', command=self.backup_now).pack(side='left')
            ttk.Button(buttons, text=_('Add a backup file…'), style='Card.TButton', command=self.add_backup_file).pack(
                side='left', padx=(px(6), 0))
            ttk.Button(buttons, text=_('Open the folder'), style='Card.TButton',
                       command=lambda: self.open_path(saves.backup_dir(self.save_user_dir()))
                       if saves.backup_dir(self.save_user_dir()).is_dir() else None).pack(side='left', padx=(px(6), 0))
        draw()
        card.redraw = draw
        return card

    def backup_row(self, body, item, state, redraw):
        tk, ttk, px = self.tk, self.ttk, self.px
        tk.Frame(body, bg=theme.BORDER, height=1).pack(fill='x', pady=(px(12), px(8)))
        line = tk.Frame(body, bg=theme.CARD)
        line.pack(fill='x')
        ttk.Button(line, text=_('Restore'), style='Card.TButton',
                   command=lambda: self.restore_backup(item)).pack(side='right', anchor='n')
        words = tk.Frame(line, bg=theme.CARD)
        words.pack(side='left', fill='x', expand=True)
        if state['rename'] == item.path:
            text = self.tk.StringVar(value=item.label or self.backup_title(item))
            entry = ttk.Entry(words, textvariable=text, width=44)
            entry.pack(anchor='w')

            def done(save=True):
                if state['rename'] != item.path:
                    return
                state['rename'] = None
                name = text.get().strip()
                if save and name != (item.label or self.backup_title(item)):
                    try:
                        saves.rename_backup(item.path, name)
                    except OSError as error:
                        self.messagebox.showerror('Bloodborne', _('Could not rename the backup: {}').format(error))
                self.root.after_idle(redraw)
            entry.bind('<Return>', lambda _e: done())
            entry.bind('<Escape>', lambda _e: done(False))
            entry.bind('<FocusOut>', lambda _e: done())
            entry.focus_set()
            entry.select_range(0, 'end')
            tk.Label(words, text=_('A name for this backup. Enter keeps it, Esc leaves it as it was.'), bg=theme.CARD,
                     fg=theme.MUTED, font=(theme.BODY, 8)).pack(anchor='w', pady=(px(2), 0))
        else:
            tk.Label(words, text=self.backup_title(item), bg=theme.CARD, fg=theme.GOLD_LIGHT,
                     font=(theme.BODY_SEMI, 10), anchor='w', justify='left', wraplength=px(520)).pack(anchor='w')
        size = item.size / 1024
        size = f'{size / 1024:.1f} MB' if size >= 1024 else f'{max(1, round(size))} KB'
        level = character.names(self.var('game_dir', 'app').get(), self.var('language', 'app').get()).word(
            character.LEVEL_TEXT)
        found = saves.backup_characters(item).values()
        who = ', '.join(f'{c.name} ({level} {c.level})' for c in found) if found else ', '.join(item.names)
        tk.Label(words, text='  ·  '.join([item.created.strftime('%d.%m.%Y %H:%M'), who or _('no characters'), size]),
                 bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 9), anchor='w', justify='left',
                 wraplength=px(520)).pack(anchor='w', pady=(px(1), 0))
        links = tk.Frame(words, bg=theme.CARD)
        links.pack(anchor='w', pady=(px(3), 0))
        for title, command in ((_('Rename'), lambda: (state.update(rename=item.path), redraw())),
                               (_('Copy to…'), lambda: self.copy_backup(item)),
                               (_('Show in folder'), lambda: self.show_backup(item)),
                               (_('Delete'), lambda: self.delete_backup(item))):
            link = tk.Label(links, text=title, bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), cursor='hand2')
            link.pack(side='left', padx=(0, px(14)))
            link.bind('<Button-1>', lambda _e, c=command: c())
            link.bind('<Enter>', lambda _e, w=link: w.configure(fg=theme.TEXT))
            link.bind('<Leave>', lambda _e, w=link: w.configure(fg=theme.MUTED))

    def backup_now(self):
        if self.game_running():
            self.messagebox.showinfo('Bloodborne', _('Close the game first; it writes its saves while it runs.'))
            return
        try:
            made = saves.backup(self.save_user_dir(), self.var('game_dir', 'app').get(), reason='manual')
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('Could not back up the saves: {}').format(error))
            return
        if not made:
            self.messagebox.showinfo('Bloodborne', _('There is no save to back up yet.'))
            return
        if getattr(self, 'backups_state', None) is not None:
            self.backups_state['rename'] = made  # name it right away (or keep the name it has)
        self.refresh_saves()

    def restore_backup(self, item):
        if self.game_running():
            self.messagebox.showinfo('Bloodborne', _('Close the game first; it writes its saves while it runs.'))
            return
        here = self.save_target()
        mine = saves.SaveSet('', here) if saves.is_save_folder(here) else None
        question = _('Put this backup back?') + '\n\n' + self.backup_title(item) + '\n' + '  ·  '.join(
            [item.created.strftime('%d.%m.%Y %H:%M'), ', '.join(item.names) or _('no characters')])
        if len(item.names) > 1:
            question += '\n\n' + _('Every character in this backup goes back to this state.')
        if mine:
            question += '\n\n' + _('Your current saves ({}) are backed up first ("{}"), so nothing is lost.').format(
                self.describe_save(mine), _(saves.REASONS['restore']))
        if not self.messagebox.askyesno('Bloodborne', question):
            return
        try:
            saves.restore(item.path, self.save_user_dir(), self.var('game_dir', 'app').get())
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('Could not put the backup back: {}').format(error))
            return
        self.messagebox.showinfo('Bloodborne', _('Backup put back. Your characters are in the game\'s Load Game menu.'))
        self.refresh_status()
        self.refresh_saves()

    def restore_character(self, item, slot, then, words):
        """One character back to its state in a backup; the other characters stay."""
        if self.game_running():
            self.messagebox.showinfo('Bloodborne', _('Close the game first; it writes its saves while it runs.'))
            return
        question = '\n'.join([
            _('Put {} back to this state?').format(then.name), '',
            item.created.strftime('%d.%m.%Y %H:%M') + '  ·  ' + self.backup_title(item),
            f'{words.word(character.LEVEL_TEXT)} {then.level}  ·  ' + _('Bosses') + f' {len(then.bosses)}/'
            f'{len(character.BOSSES)}', '',
            _('Only this character changes; your other characters stay as they are. Its current state is backed '
              'up first ("{}"), so nothing is lost.').format(_(saves.REASONS['restore']))])
        if not self.messagebox.askyesno('Bloodborne', question):
            return
        try:
            saves.restore_character(item.path, slot, self.save_user_dir(), self.var('game_dir', 'app').get())
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('This character cannot be put back on its own ({}). Restore the '
                                                      'whole backup under Backups instead.').format(error))
            return
        self.messagebox.showinfo('Bloodborne', _('{} is back at this state.').format(then.name))
        self.refresh_status()
        self.refresh_saves()

    def add_backup_file(self):
        chosen = self.filedialog.askopenfilename(title=_('Choose a backup (zip)'), initialdir=str(Path.home()),
                                                 filetypes=[(_('Backups'), '*.zip')])
        if not chosen:
            return
        try:
            saves.add_backup_file(chosen, self.save_user_dir())
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('This file is no Bloodborne save backup: {}').format(error))
            return
        self.refresh_saves()

    def copy_backup(self, item):
        target = self.filedialog.asksaveasfilename(title=_('Copy the backup to…'), initialfile=item.path.name,
                                                   defaultextension='.zip', filetypes=[(_('Backups'), '*.zip')])
        if not target:
            return
        import shutil
        try:
            shutil.copy2(item.path, target)
        except OSError as error:
            self.messagebox.showerror('Bloodborne', _('Could not copy the backup: {}').format(error))

    def show_backup(self, item):
        import subprocess
        subprocess.Popen(['explorer', '/select,', str(item.path)])

    def delete_backup(self, item):
        if not self.messagebox.askyesno('Bloodborne', _('Delete this backup? It goes to the Recycle Bin.') + '\n\n' +
                                        self.backup_title(item) + '  ·  ' + item.created.strftime('%d.%m.%Y %H:%M')):
            return
        if not saves.to_recycle_bin(item.path):
            self.messagebox.showerror('Bloodborne', _('Could not move the backup to the Recycle Bin.'))
        self.refresh_saves()

    # ---- small pieces ----------------------------------------------------------------------------
    def meter(self, parent, level, bg, width=150, height=10):
        """Five segments filled up to a rating level (1..5), blood red to gold."""
        tk, px = self.tk, self.px
        w, h, gap = px(width), px(height), px(4)
        c = tk.Canvas(parent, width=w, height=h, bg=bg, highlightthickness=0, bd=0)
        seg = (w - 4 * gap) / 5
        for i in range(5):
            color = level_color(level) if i < level else theme.BORDER
            image = self.theme.cached(('seg', round(seg), h, color, bg),
                                      lambda color=color: theme.rounded((round(seg), h), h / 2, color), bg)
            c.create_image(round(i * (seg + gap)), 0, image=image, anchor='nw')
        return c

    def state_line(self, parent, state, text, bg, wrap=520, bold=None):
        """An icon in the state's colour and a wrapped text."""
        tk, px = self.tk, self.px
        line = tk.Frame(parent, bg=bg)
        tk.Label(line, text=theme.ICON[STATE_ICONS.get(state, 'info')], bg=bg,
                 fg=STATE_COLORS.get(state, theme.MUTED), font=(theme.ICONS, 11)).pack(side='left', anchor='n',
                                                                                       pady=(px(2), 0))
        words = tk.Frame(line, bg=bg)
        words.pack(side='left', fill='x', expand=True, padx=(px(10), 0))
        if bold:
            tk.Label(words, text=bold, bg=bg, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 10), anchor='w',
                     justify='left').pack(anchor='w')
        tk.Label(words, text=text, bg=bg, fg=theme.TEXT if state != 'info' else theme.MUTED, font=(theme.BODY, 10),
                 anchor='w', justify='left', wraplength=px(wrap)).pack(anchor='w')
        return line

    def piracy_notice(self, parent, bg, wrap=640):
        """The rule against pirated game files in a box that stands out: a blood red frame and bar,
        the warning icon, a title. The caller places it."""
        tk, px = self.tk, self.px
        box = tk.Frame(parent, bg=theme.BLOOD)
        inner = tk.Frame(box, bg=bg)
        inner.pack(fill='both', expand=True, padx=1, pady=1)
        tk.Frame(inner, bg=theme.BLOOD, width=px(4)).pack(side='left', fill='y')
        tk.Label(inner, text=theme.ICON['warn'], bg=bg, fg=theme.BLOOD_LIGHT, font=(theme.ICONS, 14)).pack(
            side='left', anchor='n', padx=(px(12), 0), pady=(px(10), 0))
        words = tk.Frame(inner, bg=bg)
        words.pack(side='left', fill='both', expand=True, padx=(px(10), px(14)), pady=px(10))
        tk.Label(words, text=_('Only with your own copy of the game'), bg=bg, fg=theme.BLOOD_LIGHT, font=(theme.BODY_SEMI, 11),
                 anchor='w').pack(anchor='w')
        tk.Label(words, text=_("The port does not come with the game, and we can't give out game files or "
                               'help anyone find them. Asking for them or sharing them breaks rule 2 of the '
                               'community Discord and can get you banned.'),
                 bg=bg, fg=theme.TEXT, font=(theme.BODY, 10), justify='left', anchor='w',
                 wraplength=px(wrap)).pack(anchor='w', pady=(px(4), 0))
        return box

    def link(self, parent, text, url, bg):
        label = self.tk.Label(parent, text=text + '  ↗', bg=bg, fg=theme.GOLD_LIGHT, cursor='hand2',
                              font=(theme.BODY_MEDIUM, 9))
        label.bind('<Button-1>', lambda _e: webbrowser.open(url))
        label.bind('<Enter>', lambda _e: label.configure(fg=theme.TEXT_STRONG))
        label.bind('<Leave>', lambda _e: label.configure(fg=theme.GOLD_LIGHT))
        return label

    def gpu_choices(self):
        """(id, label) of every card in the list, the detected one first."""
        return [('', _('Detected automatically'))] + [(g.id, g.label) for g in hardware.GPU_LIST]

    def gpu_picker(self, parent, style='TCombobox', width=34):
        """A drop-down over the list for a card the detection did not find (or got wrong)."""
        var = self.var('gpu_override', 'app')
        choices = self.gpu_choices()
        box = self.ttk.Combobox(parent, values=[label for _id, label in choices], state='readonly', width=width,
                                style=style, height=18)
        ids = [i for i, _label in choices]
        box.current(ids.index(var.get()) if var.get() in ids else 0)

        def chosen(_event=None):
            var.set(ids[box.current()])
            self.update_rating()
        box.bind('<<ComboboxSelected>>', chosen)
        return box

    # ---- the rating, drawn into a card ----------------------------------------------------------
    def rating_card(self, parent, bg=theme.CARD, compact=False):
        """Verdict, the three parts and the expected frame rates; redrawn when the rating changes."""
        tk, ttk, px = self.tk, self.ttk, self.px
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(18)))
        body = tk.Frame(card, bg=bg)
        body.pack(fill='both', expand=True)

        def draw():
            for child in body.winfo_children():
                child.destroy()
            r = self.rating
            if not r:
                gpu, _manual, name = self.detected_gpu()
                if self.hw_ready and not gpu:
                    tk.Label(body, text=_('Your graphics card ({}) is not in the list of the hardware check. Pick it '
                                          'or the closest model:').format(name or '?'), bg=bg, fg=theme.TEXT,
                             font=(theme.BODY, 10), wraplength=px(560), justify='left').pack(anchor='w')
                    self.gpu_picker(body, 'Card.TCombobox').pack(anchor='w', pady=(px(10), 0))
                else:
                    tk.Label(body, text=_('Checking your PC…'), bg=bg, fg=theme.MUTED,
                             font=(theme.BODY, 10)).pack(anchor='w')
                return
            head = tk.Frame(body, bg=bg)
            head.pack(fill='x')
            left = tk.Frame(head, bg=bg)
            left.pack(side='left', fill='x', expand=True)
            tk.Label(left, text=_('Your PC').upper(), bg=bg, fg=theme.GOLD, font=(theme.HEAD, 9)).pack(anchor='w')
            tk.Label(left, text=r.tier_label, bg=bg, fg=tier_color(r.tier), font=(theme.HEAD_BOLD, 24)).pack(anchor='w')
            tk.Label(left, text=r.summary + ('  ' + _(hardware.LIMITS[r.limit])
                                             if r.limit and r.tier else ''),
                     bg=bg, fg=theme.TEXT_SOFT, font=(theme.BODY, 10), wraplength=px(640),
                     justify='left').pack(anchor='w', pady=(px(2), 0))
            table = tk.Frame(body, bg=bg)
            table.pack(fill='x', pady=(px(14), 0))
            for row, (res, level, label, settings) in enumerate(r.rows):
                tk.Label(table, text=res, bg=bg, fg=theme.TEXT_STRONG, font=(theme.HEAD, 11), width=5,
                         anchor='w').grid(row=row, column=0, sticky='w', pady=px(3))
                self.meter(table, level, bg).grid(row=row, column=1, padx=(px(8), px(12)))
                tk.Label(table, text=label, bg=bg, fg=level_color(level), font=(theme.BODY_SEMI, 10), width=13,
                         anchor='w').grid(row=row, column=2, sticky='w')
                if not compact:
                    tk.Label(table, text=settings or '—', bg=bg, fg=theme.MUTED, font=(theme.BODY, 9),
                             anchor='w').grid(row=row, column=3, sticky='w')
            tk.Frame(body, bg=theme.BORDER, height=1).pack(fill='x', pady=px(14))
            gpu, manual, name = self.detected_gpu()
            parts = {'gpu': gpu.label + (f'  ·  {gpu.vram} GB' if gpu.vram else '') +
                     ('  ' + _('(chosen by hand)') if manual else ''),
                     'cpu': self.cpu.name or _('Unknown processor'), 'ram': f'{self.ram} GB'}
            grid = tk.Frame(body, bg=bg)
            grid.pack(fill='x')
            grid.columnconfigure(1, weight=1)
            for state, part, text in r.checks:
                order = ('gpu', 'cpu', 'ram').index(part)
                tk.Label(grid, text=PART_ICONS[part], bg=bg, fg=theme.MUTED, font=(theme.ICONS, 14)).grid(
                    row=order, column=0, sticky='n', padx=(0, px(14)), pady=(px(6), 0))
                cell = tk.Frame(grid, bg=bg)
                cell.grid(row=order, column=1, sticky='we', pady=(px(4), px(6)))
                top = tk.Frame(cell, bg=bg)
                top.pack(fill='x')
                tk.Label(top, text=_(PART_TITLES[part]), bg=bg, fg=theme.MUTED, font=(theme.BODY, 9)).pack(side='left')
                tk.Label(top, text=parts[part], bg=bg, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 10)).pack(
                    side='left', padx=(px(8), 0))
                tk.Label(cell, text=theme.ICON[STATE_ICONS[state]] + '  ', bg=bg, fg=STATE_COLORS[state],
                         font=(theme.ICONS, 9)).pack(side='left', anchor='n', pady=(px(3), 0))
                tk.Label(cell, text=text, bg=bg, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), justify='left',
                         wraplength=px(560)).pack(side='left', anchor='w')
            if not compact:
                pick = tk.Frame(body, bg=bg)
                pick.pack(fill='x', pady=(px(10), 0))
                tk.Label(pick, text=_('Wrong graphics card?'), bg=bg, fg=theme.MUTED, font=(theme.BODY, 9)).pack(
                    side='left', padx=(0, px(10)))
                self.gpu_picker(pick, 'Card.TCombobox').pack(side='left')
        draw()
        self.rating_views.append(draw)
        return card

    # ---- Picture & performance ------------------------------------------------------------------------
    def build_picture(self):
        """The page every player needs for the picture: what is set for this PC, the goal, a
        measurement on this PC, the screen and the taste effects; everything else on All graphics."""
        tk, ttk, px = self.tk, self.ttk, self.px
        f = self.scrolled_page('picture', _('Picture & performance'),
                               _('The launcher sets the technical settings for your PC. You choose how it should feel; '
                                 'the in-game menu (Insert or L3+R3) changes the same values.'))
        card = ttk.Frame(f, style='Card.TFrame', padding=(px(22), px(16)))
        card.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(10), 0))
        body = tk.Frame(card, bg=theme.CARD)
        body.pack(fill='both', expand=True)
        head = tk.Frame(body, bg=theme.CARD)
        head.pack(fill='x')
        tk.Label(head, text=_('Made for your PC'), bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.HEAD, 12)).pack(
            side='left')
        state = tk.Label(head, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY_MEDIUM, 9))
        state.pack(side='left', padx=(px(14), 0), pady=(px(3), 0))
        summary = tk.Label(body, text='', bg=theme.CARD, fg=theme.TEXT_STRONG, font=(theme.BODY_SEMI, 11), anchor='w',
                           justify='left')
        summary.pack(anchor='w', pady=(px(8), 0))
        pc = tk.Label(body, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), anchor='w', justify='left',
                      wraplength=px(700))
        pc.pack(anchor='w', pady=(px(2), 0))
        goal_row = tk.Frame(body, bg=theme.CARD)
        goal_row.pack(fill='x', pady=(px(14), 0))
        tk.Label(goal_row, text=_('Your goal'), bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10)).pack(
            side='left', padx=(0, px(14)))
        self.goal_switch(goal_row).pack(side='left')
        self.index_entry(goal_row, _('Your goal'), '', 'goal')
        note = tk.Label(body, text='', bg=theme.CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), anchor='w',
                        justify='left', wraplength=px(700))
        note.pack(anchor='w', pady=(px(6), 0))
        self.goal_notes = getattr(self, 'goal_notes', []) + [note]
        buttons = tk.Frame(body, bg=theme.CARD)
        buttons.pack(fill='x', pady=(px(14), 0))
        last = None  # the last measurement, under the buttons (with the game tests on)
        if L.GAME_TESTS:
            measure = ttk.Button(buttons, text=_('Measure on this PC'), style='Card.Accent.TButton',
                                 command=lambda: self.start_benchmark('picture', 'performance'))
            measure.pack(side='left', padx=(0, px(8)))
            cancel = ttk.Button(buttons, text=_('Cancel'), style='Card.TButton', command=self.cancel_benchmark)
        details = ttk.Button(buttons, text=_('Show the settings one by one'), style='Card.TButton')
        details.pack(side='left')
        undo = ttk.Button(buttons, text=_('Undo'), style='Card.TButton',
                          command=lambda: (self.undo_profile(), draw()))
        if L.GAME_TESTS:
            last = tk.Label(body, text='', bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), anchor='w',
                            justify='left', cursor='hand2')
            last.pack(anchor='w', pady=(px(8), 0))
            last.bind('<Button-1>', lambda _e: self.open_last_report())
            bench_list = tk.Frame(body, bg=theme.CARD)
            bench_list.pack(fill='x', pady=(px(6), 0))
            bench_note = tk.Label(body, text=_('Measuring takes 2 to 10 minutes: the game starts a few times by itself '
                                               'from your last save, on a copy of it.'), bg=theme.CARD, fg=theme.MUTED,
                                  font=(theme.BODY, 9), justify='left', wraplength=px(700))
            bench_note.pack(anchor='w', pady=(px(4), 0))
            bench_apply = tk.Frame(body, bg=theme.CARD)
            bench_apply.pack(fill='x')
            self.bench_views = getattr(self, 'bench_views', {})
            self.bench_views['picture'] = {'start': measure, 'cancel': cancel, 'list': bench_list, 'note': bench_note,
                                           'apply': bench_apply}
            self.index_entry(measure, _('Measure on this PC'), _('Performance test'), 'page:picture')
        self.profile_view = self.profile_card(f)
        self.profile_view.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(12), 0))
        self.profile_view.grid_remove()

        def toggle_details():
            if self.profile_view.winfo_manager():
                self.profile_view.grid_remove()
                details.configure(text=_('Show the settings one by one'))
            else:
                self.profile_view.grid()
                self.profile_view.redraw(force=True)
                details.configure(text=_('Hide the settings'))
        self.index_entry(self.profile_view, _('Adjust them automatically when the hardware changes'),
                         _('Show the settings one by one'), 'auto_graphics',
                         reveal=lambda: None if self.profile_view.winfo_manager() else toggle_details())
        details.configure(command=toggle_details)

        def draw():
            chosen = self.recommended()
            get = lambda key, store='ini': self.var(key, store).get()
            upscaler = dict(L.UPSCALERS).get(get('upscaler'), ('?',))[0].split(' (')[0]
            preset = dict(L.PRESETS).get(get('preset'), ('',))[0].split(' (')[0]
            fps = _(*dict(L.FPS_MODES).get(get('fps_mode', 'app'), ('?',))).split(' (')[0]
            effects = sum(bool(get(k)) for k in ('effect_ssao', 'effect_dynamic_shadows', 'object_motion'))
            parts = [get('output_res').replace('x', ' × '),
                     upscaler + ('' if get('upscaler') in ('off', 'taa') else f' {preset}'), fps,
                     _('every effect') if effects == 3 else _('fewer effects') if effects else _('no costly effects')]
            summary.configure(text='  ·  '.join(parts))
            diff = self.differences() if chosen else []
            state.configure(text=(_('as recommended') if chosen and not diff else
                                  _('{} differ from the recommendation').format(len(diff)) if diff else _('checking your PC…')),
                            fg=theme.GOLD_LIGHT if chosen and not diff else theme.AMBER if diff else theme.MUTED)
            r = self.rating
            # The part that sets the limit: the processor named instead of the card when it is the one.
            pc.configure(text=(_('Your PC: {} · {}').format(r.tier_label, r.cpu.label) + '  ·  ' + _(hardware.LIMITS['cpu'])
                               if r and r.limit == 'cpu' else
                               _('Your PC: {} · {}').format(r.tier_label, r.gpu.label) if r else ''))
            if self.app.get('auto_undo'):
                undo.pack(side='left', padx=(px(8), 0))
            else:
                undo.pack_forget()
            done = self.app.get('last_measure') or {}
            if last is None:
                return
            last.configure(text=(_('Last measured on {}: {:.0f} FPS (1 % low {:.0f}) for {} FPS  ›  report').format(
                done['date'], done['fps'], done.get('low1') or 0, done['goal']) if done.get('fps') else ''))
            if done.get('fps'):
                last.pack(anchor='w', pady=(px(8), 0), before=bench_list)
            else:
                last.pack_forget()
        draw()
        self.picture_view = draw
        self.rating_views.append(draw)
        for key, store in (('upscaler', 'ini'), ('preset', 'ini'), ('output_res', 'ini'), ('fps_mode', 'app'),
                           ('effect_ssao', 'ini'), ('effect_dynamic_shadows', 'ini'), ('object_motion', 'ini')):
            self.var(key, store).trace_add('write', lambda *_a: self.root.after_idle(draw))

        self.section(f, _('Screen'))
        self.check(f, 'fullscreen', 'app', _('Fullscreen', 'Полноэкранный режим'),
                   _('Recommended: the game gets the display to itself (smoother, and 4K downsampling works).'))
        self.row(f, _('Monitor'), self.choice(f, 'game_monitor', 'app', L.monitor_options(self.app.get('game_monitor'))),
                 _('Where the game opens, in a window and in fullscreen.'))
        self.row(f, _('Resolution'), self.output_choice(f),
                 _('The size of the picture. Your goal picks it for this PC; change it here if you like.'))
        self.row(f, _('Window size'), self.window_choice(f),
                 _('In a window: the size it opens at. You can still drag its edges; "As I left it" opens it at '
                   'the size it had when you last closed the game.'))
        self.check(f, 'hdr', 'app', _('Allow HDR output', 'Разрешить HDR'),
                   _('When HDR is on in Windows and the display supports it.',
                     'Если HDR включён в Windows и монитор его поддерживает.'))
        self.section(f, _('Taste'))
        flags = {key: title for key, title, _on in L.EFFECTS}
        for key in L.TASTE_EFFECTS:
            self.check(f, key, 'ini', _(*flags[key]))
        self.check(f, 'show_fps', 'ini', _('Show the FPS counter', 'Показывать FPS'))
        self.link_row(f, _('Every graphics setting one by one:'), 'graphics')

    WINDOW_SIZES = [('output', ('Same as the output resolution',)), ('last', ('As I left it',)),
                    ('custom', ('Custom size…',)), ('game', ('As the game opens it',))]

    def window_choice(self, parent):
        """The game window's size: the output resolution, as the player left it, or their own."""
        ttk = self.ttk
        holder = ttk.Frame(parent)
        mode, own = self.var('window_size', 'app'), self.var('window_custom', 'app')
        box = self.choice(holder, 'window_size', 'app', self.WINDOW_SIZES, width=30)
        box.pack(side='left')
        note = ttk.Label(holder, style='Muted.TLabel')
        note.pack(side='left', padx=(10, 0))
        entry = self.size_entry(holder, own, lambda _size: show(), note)

        def show(*_a):
            if mode.get() == 'custom':
                entry.pack(side='left', padx=(8, 0), before=note)
            else:
                entry.pack_forget()
            last = display.parse_size(self.app.get('window_last'))
            text = (_('last time {} × {}').format(*last) if last else _('not played in a window yet')) \
                if mode.get() == 'last' else ''
            note.configure(text=text, foreground=theme.MUTED)
        mode.trace_add('write', show)
        show()
        holder.setting_key = 'window_size'
        return holder

    def open_last_report(self):
        done = self.app.get('last_measure') or {}
        report = Path(done.get('folder', '')) / 'report.html'
        if report.is_file():
            webbrowser.open(report.as_uri())

    def build_saves(self):
        px = self.px
        f = self.scrolled_page('saves', _('Saves'),
                               _('Your characters: bring them from another version, back them up, put a backup back.'))
        self.section(f, _('Your characters'), top=4)
        self.characters_view = self.characters_card(f)
        self.characters_view.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(4), 0))
        self.index_entry(self.characters_view, _('Your characters'), _('Saves'), 'characters')
        self.section(f, _('Backups'))
        self.backups_view = self.backups_card(f)
        self.backups_view.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(4), 0))
        # the card is drawn anew on every change: the search points at the card
        self.index_entry(self.backups_view, _('Back up automatically before every start'), _('Backups'), 'auto_backup')
        self.index_entry(self.backups_view, _('Back up now'), _('Backups'), 'button:backup')
        self.section(f, _('From other versions'))
        self.note(f, _('Characters from shadPS4, Awaken\'s port, bbhost or a PS4 export: found on this PC and copied '
                       'into this port\'s saves folder. Your current saves are backed up first.'), top=0)
        self.saves_card(f, manager=True).grid(row=self.next_row(f), column=0, columnspan=2, sticky='we',
                                              pady=(px(8), 0))
        self.section(f, _('Saves folder'))
        self.folder(f, 'user_dir', _('Saves folder', 'Папка сохранений'),
                    _('Choose the saves folder', 'Выберите папку сохранений'),
                    _('Your characters, their backups and the trophies. Empty: {}.').format(L.DATA_DIR / 'user'),
                    on_change=self.refresh_status)
        self.folder(f, 'cache_dir', _('Cache folder'), _('Choose the cache folder'),
                    _('Shaders, logs and temporary files of this PC. Empty: in the saves folder, or on this PC when '
                      'the saves folder is in a cloud folder (OneDrive, Dropbox, Google Drive), so every PC keeps its '
                      'own shaders.'))
        where = self.tk.Label(f, text='', bg=L.PANEL, fg=theme.MUTED, font=(theme.BODY, 9), justify='left', anchor='w')
        where.grid(row=self.next_row(f), column=1, sticky='w', pady=(4, 2))

        def show_where(*_a):
            settings = {key: self.var(key, 'app').get() for key in ('user_dir', 'cache_dir')}
            text = _('Caches now: {}').format(L.cache_folder(settings))
            if L.synced_folder(L.saves_folder(settings)):
                text += '\n' + _('Your saves folder is in a cloud folder: the saves go with you, the shaders stay on '
                                  'this PC. Play on one PC at a time and let it finish syncing before you start on '
                                  'another one; in OneDrive, set the folder to "Always keep on this device".')
            where.configure(text=text, wraplength=self.px(560))
        for key in ('user_dir', 'cache_dir'):
            self.var(key, 'app').trace_add('write', lambda *_a: self.root.after_idle(show_where))
        show_where()

    # ---- the System page --------------------------------------------------------------------------
    def build_system(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        if L.GAME_TESTS:
            f = self.scrolled_page('system', _('System & tests'),
                                   _('What your PC can do, rated like the hardware check on bbport.de, and game tests '
                                     'that measure it: the best settings for a goal, settings side by side, stability.'))
        else:
            f = self.scrolled_page('system', _('Your PC'),
                                   _('What your PC can do, rated like the hardware check on bbport.de, and the system '
                                     'requirements.'))
        rating = self.rating_card(f)
        rating.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(10), 0))
        self.index_entry(rating, _('Wrong graphics card?'), _('Detected automatically'), 'gpu_override')
        notes = tk.Frame(f, bg=L.PANEL)
        notes.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(10), 0))

        def draw_notes():
            for child in notes.winfo_children():
                child.destroy()
            for note in (self.rating.notes if self.rating else []):
                self.state_line(notes, 'info', note, L.PANEL, wrap=760).pack(fill='x', pady=px(3))
        draw_notes()
        self.rating_views.append(draw_notes)
        if L.GAME_TESTS:
            self.benchmark_card(f).grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(14), 0))

        self.section(f, _('System requirements'))
        table = ttk.Frame(f, style='Card.TFrame', padding=(px(22), px(16)))
        table.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(px(4), 0))
        grid = tk.Frame(table, bg=L.CARD)
        grid.pack(fill='x')
        grid.columnconfigure((1, 2, 3), weight=1, uniform='req')
        tk.Label(table, text=_(hardware.FOUR_K), bg=L.CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9),
                 justify='left', wraplength=px(700)).pack(anchor='w', pady=(px(10), 0))

        def draw_requirements():
            for child in grid.winfo_children():
                child.destroy()
            for col, text in enumerate(('',) + hardware.REQUIREMENT_HEADS + ('Your PC',)):
                tk.Label(grid, text=_(text) if text else '', bg=L.CARD, fg=theme.GOLD_LIGHT, font=(theme.HEAD, 10),
                         anchor='w', justify='left', wraplength=px(220)).grid(row=0, column=col, sticky='nw',
                                                                             padx=(0, px(16)), pady=(0, px(8)))
            yours = self.your_pc()
            for row, (part, low, high) in enumerate(hardware.REQUIREMENTS, start=1):
                tk.Frame(grid, bg=theme.BORDER, height=1).grid(row=row * 2 - 1, column=0, columnspan=4, sticky='we')
                tk.Label(grid, text=_(part), bg=L.CARD, fg=theme.MUTED, font=(theme.BODY_MEDIUM, 9), anchor='nw').grid(
                    row=row * 2, column=0, sticky='nw', padx=(0, px(16)), pady=px(7))
                for col, text in enumerate((low, high), start=1):
                    tk.Label(grid, text=_(text), bg=L.CARD, fg=theme.TEXT, font=(theme.BODY, 9), anchor='nw',
                             justify='left', wraplength=px(210)).grid(row=row * 2, column=col, sticky='nw',
                                                                      padx=(0, px(16)), pady=px(7))
                state, text = yours.get(part, (None, '…'))
                cell = tk.Frame(grid, bg=L.CARD)
                cell.grid(row=row * 2, column=3, sticky='nw', pady=px(7))
                if state:
                    tk.Label(cell, text=theme.ICON[STATE_ICONS[state]], bg=L.CARD, fg=STATE_COLORS[state],
                             font=(theme.ICONS, 9)).pack(side='left', anchor='n', pady=(px(2), 0), padx=(0, px(6)))
                tk.Label(cell, text=text, bg=L.CARD, fg=theme.TEXT_STRONG, font=(theme.BODY_MEDIUM, 9), anchor='nw',
                         justify='left', wraplength=px(190)).pack(side='left', anchor='nw')
        draw_requirements()
        self.rating_views.append(draw_requirements)

        self.section(f, _('Hard limits'))
        for text in hardware.HARD_LIMITS:
            self.state_line(f, 'warn', _(text), L.PANEL, wrap=760).grid(
                row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=px(3))
        self.section(f, _('Driver notes'))
        drivers = tk.Frame(f, bg=L.PANEL)
        drivers.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')

        def draw_drivers():  # the notes not already shown for this PC under its rating
            for child in drivers.winfo_children():
                child.destroy()
            shown = {before_colon(note) for note in (self.rating.notes if self.rating else [])}
            for text in hardware.DRIVER_NOTES:
                if before_colon(_(text)) not in shown:
                    self.state_line(drivers, 'info', _(text), L.PANEL, wrap=760).pack(fill='x', pady=px(3))
        draw_drivers()
        self.rating_views.append(draw_drivers)
        self.link(f, _('Install guide and FAQ on bbport.de'), L.COMMUNITY_SITE + 'install/', L.PANEL).grid(
            row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(px(16), 0))

    def your_pc(self):
        """{requirement row: (state, text)} for this PC."""
        import shutil
        import sys
        yours = {}
        build = sys.getwindowsversion().build if hasattr(sys, 'getwindowsversion') else 0
        yours['OS'] = (('ok', f'Windows {11 if build >= 22000 else 10}  ·  Build {build}') if build >= 18362 else
                       ('bad', f'Windows  ·  Build {build}'))
        if self.rating:
            checks = {part: state for state, part, _text in self.rating.checks}
            gpu = self.rating.gpu
            yours['GPU'] = (checks['gpu'], gpu.label + (f'  ·  {gpu.vram} GB' if gpu.vram else ''))
            yours['CPU'] = (checks['cpu'], self.cpu.name or _('Unknown processor'))
            yours['RAM'] = (checks['ram'], f'{self.ram} GB')
        elif self.cpu:  # no rating without the graphics card: the parts as detected
            yours['CPU'] = (None, self.cpu.name or _('Unknown processor'))
            yours['RAM'] = (None, f'{self.ram} GB')
            yours['GPU'] = (None, self.detected_gpu()[2] or '?')
        try:
            folder = Path(self.var('game_dir', 'app').get() or L.PORT_DIR)
            free = shutil.disk_usage(folder if folder.exists() else L.PORT_DIR).free / 1024 ** 3
            yours['Storage'] = ('ok' if free >= 5 else 'warn',
                                _('{} GB free on {}').format(round(free), folder.anchor or folder))
        except OSError:
            pass
        return yours

    def diagnostics(self):
        """A plain summary for asking for help (Discord, forum), in English."""
        import platform
        lines = [f'Bloodborne PC port launcher v{L.VERSION} - system check']
        gpu, manual, _name = self.detected_gpu()
        lines.append(f'GPU: {self.gpu_line or "not detected"}'
                     + (f' -> {gpu.label} ({gpu.vram} GB, score {gpu.score})' if gpu else '')
                     + (' [chosen by hand]' if manual else ''))
        if self.cpu:
            lines.append(f'CPU: {self.cpu.name} (score {self.cpu.score}, {self.cpu.support})')
        lines.append(f'RAM: {self.ram} GB')
        lines.append(f'Windows: {platform.version()}')
        lines.append('Display: {} x {}'.format(*self.display_size()))
        if self.rating:
            r = hardware.assess(self.rating.gpu, self.cpu, self.ram)  # English texts
            lines.append(f'Rating: {r.tier_label}; ' + ', '.join(f'{res} {label}' for res, _l, label, _s in r.rows))
        get = lambda key, store: self.var(key, store).get()
        lines.append('Settings: output {} | upscaler {} preset {} | fps {} | frame gen {} | fullscreen {}'.format(
            get('output_res', 'ini'), get('upscaler', 'ini'), get('preset', 'ini'), get('fps_mode', 'app'),
            get('frame_gen', 'ini'), get('fullscreen', 'app')))
        info = L.game_info(get('game_dir', 'app'))
        lines.append(f'Game: {info[0]} {info[1]}' if info else 'Game: folder not set')
        return '\n'.join(lines)

    def copy_diagnostics(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.diagnostics())
        note = getattr(self, 'report_note', None)
        if note:
            note.configure(text=_('Copied: paste it into your message on Discord or in the forum.'), fg=theme.GOLD_LIGHT)

    def relaunch(self):
        """Saves and starts the launcher again (its language applies at start)."""
        import subprocess
        import sys
        self.collect()
        command = [sys.executable] if L.FROZEN else [sys.executable, str(Path(L.__file__).resolve())]
        subprocess.Popen(command, cwd=str(L.PORT_DIR))
        self.root.destroy()

    # ---- the setup guide ----------------------------------------------------------------------------
    def maybe_open_setup(self):
        """At start: the guide for a new installation. A launcher set up before the guide existed
        (saved settings and a game folder) counts as set up."""
        if self.var('setup_done', 'app').get():
            return
        if L.CONFIG_FILE.exists() and L.game_info(self.var('game_dir', 'app').get()):
            self.var('setup_done', 'app').set(True)
            return
        self.open_setup()

    def open_setup(self):
        if not getattr(self, 'guide', None):
            self.guide = SetupGuide(self)


# ---- the game folder ------------------------------------------------------------------------------
def game_values(folder):
    """param.sfo of a dump as a dict ({} when unreadable)."""
    try:
        from prepare import sfo
        return sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError, ImportError):
        return {}


def find_game(start=''):
    """A folder with eboot.bin near the port or under `start` (CUSA03173 first), or ''."""
    places = []
    if start:
        places += [Path(start)] + _children(Path(start))
    for base in (L.PORT_DIR, L.PORT_DIR.parent):
        places += [base / 'CUSA03173'] + _children(base)
    found = [p for p in places if (p / 'eboot.bin').is_file()]
    found.sort(key=lambda p: game_values(p).get('TITLE_ID') != 'CUSA03173')
    return str(found[0]) if found else ''


def _children(folder):
    try:
        return [p for p in folder.iterdir() if p.is_dir()][:200]
    except OSError:
        return []


def game_checks(folder):
    """([(state, text)], playable) for a game folder, each problem with how to fix it."""
    folder = Path(folder) if folder else None
    if not folder or not folder.is_dir():
        return [('bad', _('No folder chosen yet. Choose your game folder: the one with eboot.bin directly '
                          'inside.'))], False
    if not (folder / 'eboot.bin').is_file():
        return [('bad', _('There is no eboot.bin in this folder. Choose the folder that has eboot.bin directly '
                          'inside, not the one above it.'))], False
    checks = []
    values = game_values(folder)
    title, version = values.get('TITLE_ID', '?'), values.get('APP_VER', '?')
    checks.append(('ok', _('eboot.bin found.')))
    if title == 'CUSA03173':
        checks.append(('ok', _('CUSA03173: the Game of the Year Edition, with The Old Hunters.')))
    else:
        checks.append(('warn', _('{}: the port is made for CUSA03173, the Game of the Year Edition. Other '
                                 'editions may not work.').format(title)))
    if version == L.PATCH_VERSION:
        checks.append(('ok', _('Version 01.09: every community patch is available.')))
    else:
        checks.append(('warn', _('Version {}: the game runs at 30 FPS only. Copy everything from the 1.09 update '
                                 'into this folder and replace the files.').format(version)))
    missing = [name for name in ('sce_module', 'sce_sys', 'dvdroot_ps4') if not (folder / name).is_dir()]
    if missing:
        checks.append(('bad', _('Missing: {}. Unpack the base game completely into this folder.').format(
            ', '.join(missing))))
    else:
        checks.append(('ok', _('sce_module, sce_sys and dvdroot_ps4 are there.')))
    port = str(L.PORT_DIR)
    if 'onedrive' in port.lower() or 'program files' in port.lower():
        checks.append(('warn', _('The port is in {}: synced folders like OneDrive and Program Files cause '
                                 'problems. Move it to a plain folder like C:\\Games\\BBPort.').format(port)))
    return checks, not missing


# ---- the guide ------------------------------------------------------------------------------------
STEPS = [('welcome', 'Awakening', 'Welcome'), ('check', 'Blood test', 'Check the game'),
         ('picture', "Hunter's sight", 'Picture & controls'), ('carry', 'The Dream', 'Saves & online'),
         ('ready', 'The hunt', 'Ready to play')]
NUMERALS = ('I', 'II', 'III', 'IV', 'V')


class SetupGuide:
    """The first start, over the whole window: a rail with the emblem that fills with blood step by
    step, and one page per step. Back and Next (Esc / Enter); the launcher is right behind it."""

    def __init__(self, app):
        self.app, self.tk, self.ttk, self.px = app, app.tk, app.ttk, app.px
        tk, px = self.tk, self.px
        self.step, self.fill, self.phase, self.choice, self.closed = 0, 0.0, 0.0, None, False
        self.fsr4_download = tk.BooleanVar(value=True)
        self.frame_gen = tk.BooleanVar(value=False)
        self.fullscreen_offered = False  # the picture step turns fullscreen on once (its default)
        self.online_offered = False  # the online step turns online play on once (its default)
        self.origin = tk.StringVar(value='new')  # where the player comes from (step I)
        self.mode = tk.StringVar(value=app.app.get('ui_mode') or 'simple')  # the launcher's view (step V)
        # Repeated from Help: the player's own choices (online, fullscreen, key bindings) are only
        # changed when they say so; the first setup sets the defaults.
        self.first = not app.var('setup_done', 'app').get()
        style = self.ttk.Style()
        style.configure('Bar.Accent.TButton', background=theme.DEEP, padding=(px(26), px(9)), font=(theme.BODY_SEMI, 11))
        style.configure('Big.Bar.TButton', background=theme.DEEP, padding=(px(18), px(9)), font=(theme.BODY_MEDIUM, 10))

        self.frame = tk.Frame(app.root, bg=theme.BG)
        top = app.titlebar_height() if hasattr(app, 'titlebar_height') else 0  # below the launcher's title bar
        self.frame.place(x=0, y=top, relwidth=1, relheight=1, height=-top)
        self.frame.lift()
        rail = tk.Frame(self.frame, bg=theme.DEEP, width=px(292))
        rail.pack(side='left', fill='y')
        rail.pack_propagate(False)
        tk.Frame(self.frame, bg=theme.BORDER, width=1).pack(side='left', fill='y')
        self.mark = 140 if app.compact else 196  # the emblem's height
        self.emblem = tk.Canvas(rail, width=px(292), height=px(self.mark + 16), bg=theme.DEEP, highlightthickness=0, bd=0)
        self.emblem.pack(pady=(px(14 if app.compact else 26), 0))
        tk.Label(rail, text=_('FIRST AWAKENING'), bg=theme.DEEP, fg=theme.GOLD, font=(theme.HEAD, 9)).pack()
        tk.Label(rail, text=_('Setup'), bg=theme.DEEP, fg=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 19)).pack(
            pady=(0, px(18)))
        self.steps = tk.Canvas(rail, width=px(244), height=px(5 * 58), bg=theme.DEEP, highlightthickness=0, bd=0)
        self.steps.pack(padx=px(24))
        tk.Frame(rail, bg=theme.DEEP).pack(fill='both', expand=True)
        skip = tk.Label(rail, text=_('Skip: I set everything up myself'), bg=theme.DEEP, fg=theme.MUTED,
                        font=(theme.BODY, 9), cursor='hand2')
        skip.pack(pady=(0, px(22)))
        skip.bind('<Button-1>', lambda _e: self.finish(play=False, skipped=True))
        skip.bind('<Enter>', lambda _e: skip.configure(fg=theme.TEXT_STRONG))
        skip.bind('<Leave>', lambda _e: skip.configure(fg=theme.MUTED))

        right = tk.Frame(self.frame, bg=theme.BG)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=theme.DEEP, height=px(80))
        bar.pack(side='bottom', fill='x')
        bar.pack_propagate(False)
        tk.Frame(right, bg=theme.BORDER, height=1).pack(side='bottom', fill='x')
        self.counter = tk.Label(bar, text='', bg=theme.DEEP, fg=theme.MUTED, font=(theme.HEAD, 10))
        self.counter.pack(side='left', padx=px(28))
        self.next_button = self.ttk.Button(bar, text='', style='Bar.Accent.TButton', command=self.next)
        self.next_button.pack(side='right', padx=(px(10), px(24)))
        self.back_button = self.ttk.Button(bar, text=_('Back', 'Назад'), style='Big.Bar.TButton', command=self.back)
        self.back_button.pack(side='right')
        self.head = tk.Canvas(right, height=px(132), bg=theme.BG, highlightthickness=0, bd=0)
        self.head.pack(fill='x')
        self.head.bind('<Configure>', lambda _e: self.draw_head())
        holder = tk.Frame(right, bg=theme.BG)
        holder.pack(fill='both', expand=True)
        self.canvas = tk.Canvas(holder, bg=theme.BG, highlightthickness=0, bd=0)
        scroll = self.ttk.Scrollbar(holder, orient='vertical', command=self.canvas.yview)
        self.body = tk.Frame(self.canvas, bg=theme.BG, padx=px(40), pady=px(6))
        window = self.canvas.create_window(0, 0, window=self.body, anchor='nw')
        self.body.bind('<Configure>', lambda _e: self.canvas.configure(scrollregion=self.canvas.bbox('all')))
        self.canvas.bind('<Configure>', lambda e: self.canvas.itemconfigure(window, width=e.width))
        self.canvas.configure(yscrollcommand=scroll.set, yscrollincrement=1)
        scroll.pack(side='right', fill='y', padx=(0, px(4)), pady=px(6))
        self.canvas.pack(side='left', fill='both', expand=True)
        root = app.root
        root.bind_all('<MouseWheel>', self.wheel)
        root.bind_all('<Return>', lambda _e: self.next())
        root.bind_all('<Escape>', lambda _e: self.back())
        self.wave_until = 0.0
        self.show(0)

    # -- frame --
    def wheel(self, event):
        if 'popdown' in str(event.widget):
            return
        self.wheel_pixels = getattr(self, 'wheel_pixels', 0.0) - event.delta / 120 * self.px(64)
        if not getattr(self, 'wheel_pending', None):
            self.wheel_pending = self.app.root.after_idle(self.flush_wheel)

    def flush_wheel(self):
        self.wheel_pending = None
        pixels = int(self.wheel_pixels)
        self.wheel_pixels -= pixels
        if pixels and not self.closed and self.body.winfo_height() > self.canvas.winfo_height():
            self.canvas.yview_scroll(pixels, 'units')

    def animate(self):
        """The blood rises to the current step and its surface waves for a moment; then the
        emblem stands still (no work while the guide only waits for the player)."""
        self.animating = None
        if self.closed:
            return
        target = self.step / (len(STEPS) - 1)
        moving = abs(target - self.fill) > 0.004
        self.fill = self.fill + (target - self.fill) * 0.18 if moving else target
        self.phase += 0.12
        image = theme.emblem_fill(self.px(self.mark), self.fill, self.phase)
        self.emblem_image = ImageTk.PhotoImage(theme.solid(image, theme.DEEP), master=self.app.root)
        if not self.emblem.find_all():
            self.emblem.create_image(self.px(146), self.px(self.mark / 2 + 8), image=self.emblem_image, tags='mark')
        else:
            self.emblem.itemconfigure('mark', image=self.emblem_image)
        if moving or time.monotonic() < self.wave_until:
            self.animating = self.app.root.after(40 if moving else 70, self.animate)

    def wake(self):
        """Starts the emblem's animation for a step change."""
        self.wave_until = time.monotonic() + 2.5
        if not getattr(self, 'animating', None):
            self.animate()

    def draw_steps(self):
        c, px = self.steps, self.px
        c.delete('all')
        r = px(15)
        for i, (_key, lore, plain) in enumerate(STEPS):
            y = px(18) + i * px(58)
            if i < len(STEPS) - 1:
                c.create_line(r, y + r, r, y + px(58) - r, fill=theme.GOLD if i < self.step else theme.BORDER_STRONG,
                              width=max(1, px(1.5)))
            state = 'done' if i < self.step else 'now' if i == self.step else 'later'
            fill, line, fg = {'done': (theme.CARD, theme.GOLD, theme.GOLD_LIGHT),
                              'now': (theme.BLOOD, theme.BLOOD_LIGHT, theme.TEXT_STRONG),
                              'later': (theme.DEEP, theme.BORDER_STRONG, theme.MUTED)}[state]
            image = self.app.theme.cached(('step', r, fill, line), lambda fill=fill, line=line: theme.rounded(
                (2 * r, 2 * r), r, fill, line, 1.5 * self.app.dpi), theme.DEEP)
            tag = f'step{i}'
            c.create_image(0, y, image=image, anchor='nw', tags=tag)
            if state == 'done':
                c.create_text(r, y + r, text=theme.ICON['ok'], fill=fg, font=(theme.ICONS, 10), tags=tag)
            else:
                c.create_text(r, y + r, text=NUMERALS[i], fill=fg, font=(theme.HEAD_BOLD, 9), tags=tag)
            c.create_text(2 * r + px(14), y + px(6), text=_(lore), anchor='nw', tags=tag,
                          fill=theme.TEXT_STRONG if state == 'now' else theme.TEXT_SOFT if state == 'done' else theme.MUTED,
                          font=(theme.HEAD, 11))
            c.create_text(2 * r + px(14), y + px(26), text=_(plain), anchor='nw', fill=theme.MUTED,
                          font=(theme.BODY, 9), tags=tag)
            if i < self.step:
                c.tag_bind(tag, '<Button-1>', lambda _e, i=i: self.show(i))
                c.tag_bind(tag, '<Enter>', lambda _e: c.configure(cursor='hand2'))
                c.tag_bind(tag, '<Leave>', lambda _e: c.configure(cursor=''))

    def draw_head(self):
        c = self.head
        w, h = max(c.winfo_width(), 300), int(c['height'])
        key, lore, _plain = STEPS[self.step]
        title, subtitle = self.titles[key]
        art = 'hero.jpg' if key in ('welcome', 'ready') else 'header.jpg'
        focus = (0.5, 0.78) if art == 'hero.jpg' else (0.72, 0.42)
        c.delete('all')
        c.art = self.app.theme.cached(('guide-head', art, w, h),
                                      lambda: theme.header((w, h), theme.BG, art, focus))
        c.create_image(0, 0, image=c.art, anchor='nw')
        px = self.px
        c.create_text(px(40), px(36), text=f'{NUMERALS[self.step]}  ·  {_(lore)}'.upper(), anchor='w', fill=theme.GOLD,
                      font=(theme.HEAD, 9))
        c.create_text(px(39), px(66), text=title, anchor='w', fill=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 24))
        c.create_text(px(40), px(96), text=subtitle, anchor='nw', fill=theme.TEXT_SOFT, font=(theme.BODY, 10),
                      width=w - px(80))

    @property
    def titles(self):
        return {
            'welcome': (_('Welcome, good hunter'), _('A few steps to your first hunt. Every setting can be changed '
                                                     'in the launcher afterwards.')),
            'check': (_('Your game and your PC'), _('Checked before the first start, so that it does not fail on '
                                                    'something you can fix now. Nothing leaves your PC.')),
            'picture': (_('Picture and controls'), _('Choose how it should feel; the settings for it come from your '
                                                     'hardware.')),
            'carry': (_('Your characters and online play'), _('Bring your saves along, and play with others.')),
            'ready': (_('The hunt begins'), _('Everything is set. Press PLAY, or look around the launcher first.')),
        }

    # -- steps --
    def show(self, step):
        self.step = step
        for child in self.body.winfo_children():
            child.destroy()
        self.canvas.yview_moveto(0)
        key = STEPS[step][0]
        getattr(self, 'step_' + key)(self.body)
        self.counter.configure(text=_('Step {} of {}').format(step + 1, len(STEPS)))
        self.back_button.configure(state='normal' if step else 'disabled')
        if key == 'ready':
            self.next_button.configure(text=_('PLAY', 'ИГРАТЬ'), style='Play.TButton')
        else:
            self.next_button.configure(text=_('Next', 'Далее') + '   ›', style='Bar.Accent.TButton')
        self.draw_steps()
        self.draw_head()
        self.update_next()
        self.wake()

    def update_next(self):
        if STEPS[self.step][0] == 'check':
            self.next_button.configure(state='normal' if self.game_ok else 'disabled')
        else:
            self.next_button.configure(state='normal')

    def next(self):
        if self.closed or str(self.next_button.cget('state')) == 'disabled':
            return
        key = STEPS[self.step][0]
        if key == 'picture':
            self.apply_graphics()
        if key == 'ready':
            self.finish(play=True)
        else:
            self.show(self.step + 1)

    def back(self):
        if not self.closed and self.step:
            self.show(self.step - 1)

    def finish(self, play, skipped=False):
        app = self.app
        self.closed = True
        app.var('setup_done', 'app').set(True)
        app.app['seen_version'] = L.VERSION  # a new installation needs no news
        if not skipped:
            app.app['ui_mode'] = self.mode.get()
            app.expert_var.set(self.mode.get() == 'expert')
            app.apply_mode()
        if app.rating and not app.app.get('auto_hw'):  # skipped: the hardware as it is now, nothing changed
            app.app['auto_hw'] = hardware.signature(app.gpu_line, app.cpu, app.ram, app.display_size())
        try:
            app.collect()
        except Exception:  # the launcher must never stay hidden behind the guide
            pass
        root = app.root
        root.unbind_all('<Return>')
        root.unbind_all('<Escape>')
        root.bind_all('<MouseWheel>', app.wheel)
        self.frame.destroy()
        app.guide = None
        app.game_changed()
        app.show('play')
        if not skipped and self.fsr4_download.get() and app.var('upscaler', 'ini').get() == 'fsr4' and L.fsr4_missing():
            app.download_fsr4()
        if play and L.game_info(app.var('game_dir', 'app').get()):
            app.play()

    # -- pieces --
    def card(self, parent, pady=(0, 0)):
        frame = self.ttk.Frame(parent, style='Card.TFrame', padding=(self.px(22), self.px(18)))
        frame.pack(fill='x', pady=pady)
        inner = self.tk.Frame(frame, bg=theme.CARD)
        inner.pack(fill='both', expand=True)
        return inner

    def text(self, parent, text, bg=theme.BG, size=10, fg=theme.TEXT_SOFT, wrap=680, font=None):
        label = self.tk.Label(parent, text=text, bg=bg, fg=fg, font=font or (theme.BODY, size), justify='left',
                              anchor='w', wraplength=self.px(wrap))
        label.pack(anchor='w', fill='x')
        return label

    def heading(self, parent, text, bg=theme.CARD):
        self.tk.Label(parent, text=text, bg=bg, fg=theme.GOLD_LIGHT, font=(theme.HEAD, 12)).pack(
            anchor='w', pady=(0, self.px(8)))

    # -- I: welcome --
    ORIGINS = [('new', 'I am new to the port'), ('shadps4', 'shadPS4'), ('bbhost', 'bbhost'),
               ('awaken', "Awaken's port"), ('ps4', 'A PS4 (exported save)')]

    def step_welcome(self, body):
        px = self.px
        self.text(body, _('A few steps to your first hunt: the guide checks your game and your PC, sets the picture '
                          'for your hardware and brings your character along. Every setting can be changed in the '
                          'launcher afterwards.'), size=11, wrap=720)
        row = self.tk.Frame(body, bg=theme.BG)
        row.pack(fill='x', pady=(px(18), 0))
        lang = self.tk.Frame(row, bg=theme.BG)
        lang.pack(side='left')
        self.tk.Label(lang, text=_('Launcher language', 'Язык лаунчера'), bg=theme.BG, fg=theme.TEXT,
                      font=(theme.BODY, 10)).pack(side='left', padx=(0, px(12)))
        before = self.app.var('ui_language', 'app').get()
        self.app.choice(lang, 'ui_language', 'app', L.UI_LANGUAGES, 24).pack(side='left')
        apply = self.ttk.Button(lang, text=_('Apply (restarts the launcher)'), command=self.app.relaunch)

        def watch(*_a):
            if not apply.winfo_exists():
                return
            if self.app.var('ui_language', 'app').get() != before:
                apply.pack(side='left', padx=(px(10), 0))
            else:
                apply.pack_forget()
        self.app.var('ui_language', 'app').trace_add('write', watch)
        self.app.piracy_notice(body, theme.BG, wrap=660).pack(fill='x', pady=(px(18), 0))  # first, not below the fold
        origin = self.card(body, pady=(px(14), 0))
        self.heading(origin, _('Where do you come from?'))
        for value, text in self.ORIGINS:
            self.ttk.Radiobutton(origin, text=_(text), variable=self.origin, value=value,
                                 style='Card.TRadiobutton').pack(anchor='w', pady=px(2))
        self.tk.Label(origin, text=_('Coming from another version, your characters are found in step IV.'),
                      bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9)).pack(anchor='w', pady=(px(6), 0))
        need = self.card(body, pady=(px(14), 0))
        self.heading(need, _('What you need'))
        for state, text in (('ok', _('Your own Bloodborne game files (CUSA03173, Game of the Year Edition) '
                                     'with the 1.09 update.')),
                            ('ok', _('Windows 10 or 11 (64-bit) and a graphics card with Vulkan 1.3 and at least '
                                     '4 GB of video memory.'))):
            self.app.state_line(need, state, text, theme.CARD, wrap=660).pack(fill='x', pady=px(3))

    # -- II: check the game (and the PC) --
    def step_check(self, body):
        """The game folder and every check of the Help page, each problem with its fix right here;
        the PC's rating in a line. Next waits for what would stop the game for sure."""
        app, tk, ttk, px = self.app, self.tk, self.ttk, self.px
        import bbport_help
        var = app.var('game_dir', 'app')
        found_note = ''
        if not L.game_info(var.get()):
            found = find_game()
            if found:
                var.set(found)
                found_note = _('Found automatically: {}').format(found)
        self.text(body, _('Your game folder: the one with eboot.bin, sce_module, sce_sys and dvdroot_ps4 '
                          'directly inside.'), size=10, wrap=720)
        row = tk.Frame(body, bg=theme.BG)
        row.pack(fill='x', pady=(px(12), 0))
        ttk.Entry(row, textvariable=var).pack(side='left', fill='x', expand=True)

        def browse():
            chosen = app.filedialog.askdirectory(title=_('Choose the folder with eboot.bin'),
                                                 initialdir=var.get() or str(L.PORT_DIR))
            if not chosen:
                return
            folder = Path(chosen)
            if not (folder / 'eboot.bin').is_file():
                inside = find_game(chosen)
                if inside and Path(inside).parent == folder:
                    folder = Path(inside)
                    self.found.configure(text=_('Used the subfolder {} (it has eboot.bin).').format(folder.name))
            var.set(str(folder))
        ttk.Button(row, text=_('Browse…', 'Обзор…'), style='Accent.TButton', command=browse).pack(
            side='left', padx=(px(8), 0))
        self.found = tk.Label(body, text=found_note, bg=theme.BG, fg=theme.GOLD_LIGHT, font=(theme.BODY_MEDIUM, 9),
                              anchor='w', justify='left')
        self.found.pack(anchor='w', pady=(px(6), 0))
        checks = self.card(body, pady=(px(8), 0))
        pc = self.card(body, pady=(px(12), 0))
        prepare = self.card(body, pady=(px(12), 0))
        self.game_ok = False
        self.check_token = 0

        def problem(parent, state, topic, text):
            app.state_line(parent, state, text, theme.CARD, wrap=640).pack(fill='x', pady=(px(3), 0))
            if not topic:
                return
            title, why, steps = bbport_help.TOPICS[topic]
            box = tk.Frame(parent, bg=theme.SURFACE, padx=px(14), pady=px(8))
            box.pack(fill='x', padx=(px(26), 0), pady=(px(4), px(6)))
            for number, step in enumerate(steps, 1):
                tk.Label(box, text=f'{number}.  ' + _(step), bg=theme.SURFACE, fg=theme.TEXT, font=(theme.BODY, 9),
                         justify='left', anchor='w', wraplength=px(600)).pack(anchor='w', pady=px(1))
            buttons = tk.Frame(box, bg=theme.SURFACE)
            buttons.pack(anchor='w', pady=(px(6), 0))
            for label, method in bbport_help.TOPIC_ACTIONS.get(topic, []):
                ttk.Button(buttons, text=_(label), style='Card.TButton',
                           command=getattr(app, method)).pack(side='left', padx=(0, px(8)))
            ttk.Button(buttons, text=_('Check again'), style='Card.TButton', command=refresh).pack(side='left')

        def refresh(*_a):
            if self.closed or not checks.winfo_exists():
                return
            for child in checks.winfo_children():
                child.destroy()
            self.heading(checks, _('Your game'))
            rows, ok = game_checks(var.get())
            if not ok:
                for state, text in rows:
                    problem(checks, state, None, text)
                self.game_ok = False
                self.update_next()
                return
            app.state_line(checks, 'info', _('Checking the game files…'), theme.CARD).pack(fill='x', pady=px(3))
            self.game_ok = False
            self.update_next()
            self.check_token += 1
            token = self.check_token
            args = (var.get(), app.var('mods_dir', 'app').get() or str(L.DATA_DIR / 'mods'), L.PORT_DIR,
                    app.var('output_res', 'ini').get(), app.app.get('eboot_check') or {})

            def work():
                try:
                    result = bbport_help.run_checks(*args)
                except Exception as error:  # a check must never stop the guide
                    result = ([('warn', None, _('The check stopped: {}').format(error))], None)
                app.ui_calls.put(lambda: done(token, *result))
            import threading
            threading.Thread(target=work, daemon=True).start()

        def done(token, results, eboot):
            if self.closed or token != self.check_token or not checks.winfo_exists():
                return
            if eboot:
                app.app['eboot_check'] = eboot
            for child in checks.winfo_children():
                child.destroy()
            self.heading(checks, _('Your game'))
            rows, _ok = game_checks(var.get())
            for state, text in rows[:2]:  # eboot.bin found, the edition
                app.state_line(checks, state, text, theme.CARD, wrap=640).pack(fill='x', pady=(px(3), 0))
            blocking = False
            for state, topic, text in results:
                problem(checks, state, topic if state != 'ok' else None, text)
                blocking = blocking or (state == 'bad' and topic in ('eboot_truncated', 'eboot_unreadable',
                                                                     'update_only', 'empty_files', 'eboot_modified'))
            self.game_ok = not blocking
            if blocking:
                tk.Label(checks, text=_('Next is possible once the game files are right: without that the game would '
                                        'not start or would crash.'), bg=theme.CARD, fg=theme.AMBER,
                         font=(theme.BODY, 9), justify='left', wraplength=px(640)).pack(anchor='w', pady=(px(8), 0))
            self.update_next()
            app.refresh_status()
        trace = var.trace_add('write', refresh)
        checks.bind('<Destroy>', lambda _e: var.trace_remove('write', trace) if trace in
                    [t[1] for t in var.trace_info()] else None)
        refresh()

        def draw_pc():
            if self.closed or not pc.winfo_exists():
                return
            for child in pc.winfo_children():
                child.destroy()
            self.heading(pc, _('Your PC'))
            r = app.rating
            if not r:
                gpu = app.detected_gpu()[0]
                if app.hw_ready and not gpu:
                    tk.Label(pc, text=_('Your graphics card is not in the list of the hardware check. Pick it or the '
                                        'closest model:'), bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10),
                             wraplength=px(620), justify='left').pack(anchor='w')
                    app.gpu_picker(pc, 'Card.TCombobox').pack(anchor='w', pady=(px(8), 0))
                else:
                    app.state_line(pc, 'info', _('Checking your PC…'), theme.CARD).pack(fill='x')
                return
            state = 'bad' if r.tier == 0 else 'warn' if r.tier == 1 else 'ok'
            app.state_line(pc, state, f'{r.tier_label}: {r.summary}', theme.CARD, wrap=640).pack(fill='x', pady=px(2))
            for part_state, _part, text in r.checks:
                if part_state != 'ok':
                    app.state_line(pc, part_state, text, theme.CARD, wrap=640).pack(fill='x', pady=px(2))
            if r.tier == 0:
                app.state_line(pc, 'bad', _('This PC does not meet the requirements. You can still go on, but the game '
                                            'will probably not run.'), theme.CARD, wrap=640).pack(fill='x', pady=px(2))
        draw_pc()
        app.rating_views.append(draw_pc)
        self.heading(prepare, _('How to prepare the game folder (once)'))
        for number, text in enumerate((
                _('Put the base game and the 1.09 update into two new, empty folders.'),
                _('Copy everything from the update folder into the base game folder and choose "Replace the files '
                  'in the destination".'),
                _('Check the result: eboot.bin (about 89 MB), sce_module, sce_sys and dvdroot_ps4 (about 29 GB) '
                  'are directly inside. The Old Hunters is already part of the GOTY edition.')), start=1):
            line = tk.Frame(prepare, bg=theme.CARD)
            line.pack(fill='x', pady=px(3))
            tk.Label(line, text=NUMERALS[number - 1], bg=theme.CARD, fg=theme.GOLD, font=(theme.HEAD_BOLD, 10),
                     width=3, anchor='nw').pack(side='left', anchor='n')
            tk.Label(line, text=text, bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY, 10), justify='left',
                     anchor='w', wraplength=px(640)).pack(side='left', fill='x')

    # -- III: picture and controls --
    def step_picture(self, body):
        app, tk, ttk, px = self.app, self.tk, self.ttk, self.px
        if not self.fullscreen_offered and self.first:
            self.fullscreen_offered = True
            app.var('fullscreen', 'app').set(True)
        goal = self.card(body)
        self.heading(goal, _('Your goal'))
        app.goal_switch(goal).pack(anchor='w')
        note = tk.Label(goal, text='', bg=theme.CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9), justify='left',
                        wraplength=px(660))
        note.pack(anchor='w', pady=(px(6), 0))
        app.goal_notes = [n for n in getattr(app, 'goal_notes', []) if n.winfo_exists()] + [note]
        app.show_goal()
        r = app.rating
        width, height = app.display_size()
        if r:
            available = hardware.outputs(width, height)
            recommended = hardware.recommended_class(r, available)
            if self.choice not in available:
                self.choice = recommended
            self.text(body, _('Your display: {} × {}. Recommended for your PC: {}.').format(
                width, height, recommended), size=10, fg=theme.TEXT_SOFT).pack_configure(pady=(px(16), 0))
            cards = tk.Frame(body, bg=theme.BG)
            cards.pack(fill='x', pady=(px(10), 0))
            self.cards = {}
            for res in hardware.RESOLUTIONS:
                if res in available:
                    self.cards[res] = self.res_card(cards, res, available[res], res == recommended)
                    self.cards[res].pack(side='left', padx=(0, px(14)))
            self.paint_cards()
        else:
            self.text(body, _('Your graphics card is not rated yet (see the previous step), so your current '
                              'graphics settings stay as they are.'), wrap=720)
        options = self.card(body, pady=(px(16), 0))
        self.heading(options, _('Options'))
        ttk.Checkbutton(options, text=_('Fullscreen (recommended)'), variable=app.var('fullscreen', 'app'),
                        style='Card.TCheckbutton').pack(anchor='w')
        if r and r.frame_gen:
            name = 'DLSS' if r.frame_gen == 'dlss' else 'FSR 3.1'
            ttk.Checkbutton(options, text=_('{} frame generation: higher frame rates, a little more input '
                                            'latency').format(name), variable=self.frame_gen,
                            style='Card.TCheckbutton').pack(anchor='w')
        if r and hardware.upscaler_name(r.gpu) == 'FSR 4' and L.fsr4_missing():
            ttk.Checkbutton(options, text=_('Download the FSR 4 files now (about 30 MB)'), variable=self.fsr4_download,
                            style='Card.TCheckbutton').pack(anchor='w')
        device = self.card(body, pady=(px(14), 0))
        self.heading(device, _('What do you play with?'))
        # The launcher's own setting (also on Play and Controls): applied at once (set_play_with).
        for value, text in (('controller', _('A controller (PlayStation, Xbox, Switch)')),
                            ('keyboard', _('Keyboard and mouse: the Dark Souls III layout with the mouse camera'))):
            ttk.Radiobutton(device, text=text, variable=app.var('play_with', 'app'), value=value,
                            command=lambda v=value: app.set_play_with(v),
                            style='Card.TRadiobutton').pack(anchor='w', pady=px(2))
        tk.Label(device, text=_('The game shows the buttons of whatever you use; the keys can be changed on the '
                                'Controls page.'), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9),
                 justify='left', wraplength=px(660)).pack(anchor='w', pady=(px(6), 0))

    # -- IV: saves and online --
    def step_carry(self, body):
        app, tk, ttk, px = self.app, self.tk, self.ttk, self.px
        origin = self.origin.get()
        played = app.saves_card(body, limit=4, title=_('Your characters from other versions'),
                                hide_empty=origin == 'new')
        played.pack_options = {'fill': 'x', 'pady': (0, px(14))}
        if origin != 'new':
            played.pack(fill='x', pady=(0, px(14)))
        if not self.online_offered and self.first:  # on by default (The Hunter's Dream), once
            self.online_offered = True
            app.var('online', 'app').set(True)
            if L.HUNTERS_DREAM_POOL:
                app.var('online_pool', 'app').set('huntersdream')
        online = self.card(body)
        played.pack_options['before'] = online.master  # found later: above the online card
        self.heading(online, _('Play with others'))
        ttk.Checkbutton(online, text=_('Play online: messages, bloodstains, summons and co-op'),
                        variable=app.var('online', 'app'), style='Card.TCheckbutton').pack(anchor='w')
        pool = app.var('online_pool', 'app')
        if L.HUNTERS_DREAM_POOL and pool.get() == 'huntersdream':
            tk.Label(online, text=_("With The Hunter's Dream, together with bbhost players. It needs a free sign-in "
                                    'with Discord, once.'), bg=theme.CARD, fg=theme.TEXT_SOFT, font=(theme.BODY, 9),
                     justify='left', wraplength=px(660)).pack(anchor='w', pady=(px(6), 0))
            status = tk.Label(online, text='', bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_SEMI, 10))
            status.pack(anchor='w', pady=(px(8), 0))
            code = tk.Label(online, text='', bg=theme.CARD, fg=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 18))
            row = tk.Frame(online, bg=theme.CARD)
            row.pack(anchor='w', pady=(px(8), 0))
            thd_id, thd_token = app.var('thd_online_id', 'app'), app.var('thd_token', 'app')

            def show():
                if status.winfo_exists():
                    status.configure(text=_('Signed in as {}').format(thd_id.get()) if thd_token.get() else
                                     _('Not signed in yet'), fg=theme.GOLD_LIGHT if thd_token.get() else theme.AMBER)
                    button.configure(text=_('Sign in again') if thd_token.get() else _('Sign in with Discord'),
                                     style='Card.TButton' if thd_token.get() else 'Card.Accent.TButton')

            def sign_in():
                status.configure(text=_('Starting the sign-in…'), fg=theme.MUTED)
                game = app.var('online_community', 'app').get()

                def on_code(page, text):
                    app.ui_calls.put(lambda: (status.configure(
                        text=_('In the page that opened, sign in with Discord and enter this code:'), fg=theme.TEXT),
                        code.configure(text=text), code.pack(anchor='w', pady=(px(4), 0), before=row)))
                    import webbrowser
                    webbrowser.open(page)

                def work():
                    try:
                        name, token = L.thd_sign_in(game, on_code)
                    except (OSError, ValueError) as error:
                        app.ui_calls.put(lambda e=error: status.configure(
                            text=_('Sign-in failed: {}').format(e), fg=theme.BLOOD_LIGHT))
                        return
                    app.ui_calls.put(lambda: (thd_id.set(name), thd_token.set(token), code.pack_forget(), show(),
                                              app.collect()))
                import threading
                threading.Thread(target=work, daemon=True).start()
            button = ttk.Button(row, text=_('Sign in with Discord'), style='Card.Accent.TButton', command=sign_in)
            button.pack(side='left')
            show()
        tk.Label(online, text=_('A shadNet server instead, and every option: the Online play page. Offline needs '
                                'nothing: untick it.'), bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9),
                 justify='left', wraplength=px(660)).pack(anchor='w', pady=(px(10), 0))

    # -- V: ready --
    def step_ready(self, body):
        app, tk, ttk, px = self.app, self.tk, self.ttk, self.px
        summary = self.card(body)
        self.heading(summary, _('Your setup'))
        grid = tk.Frame(summary, bg=theme.CARD)
        grid.pack(fill='x')
        grid.columnconfigure(1, weight=1)
        get = lambda key, store: app.var(key, store).get()
        info = L.game_info(get('game_dir', 'app'))
        upscaler = dict(L.UPSCALERS).get(get('upscaler', 'ini'), ('?',))[0].split(' (')[0]
        preset = dict(L.PRESETS).get(get('preset', 'ini'), ('',))[0].split(' (')[0]
        fps = dict(L.FPS_MODES).get(get('fps_mode', 'app'), ('?',))
        goal = app.goal_name(get('goal', 'app'))
        online = (_('On, signed in as {}').format(get('thd_online_id', 'app')) if get('online', 'app') and
                  get('thd_token', 'app') else _('On, sign-in still missing') if get('online', 'app') else _('Off'))
        rows = [(theme.ICON['folder'], _('Game'), (_('{} · version {}').format(get('game_dir', 'app'), info[1])
                                                   if info else _('not set')), None),
                (theme.ICON['display'], _('Picture'), '  ·  '.join(
                    [goal, get('output_res', 'ini').replace('x', ' × '),
                     upscaler + (f' {preset}' if get('upscaler', 'ini') not in ('off', 'taa') else ''),
                     _(*fps), _('Fullscreen', 'Полный экран') if get('fullscreen', 'app') else _('Window')]), None),
                (theme.ICON['online'], _('Online'), online, theme.AMBER if get('online', 'app') and not
                 get('thd_token', 'app') and get('online_pool', 'app') == 'huntersdream' else None)]
        expected = app.expectation()
        if expected:
            rows.append((theme.ICON['system'], _('Expected'), after_colon(expected[0]), expected[1]))
        for row, (icon, title, value, color) in enumerate(rows):
            tk.Label(grid, text=icon, bg=theme.CARD, fg=theme.MUTED, font=(theme.ICONS, 13)).grid(
                row=row, column=0, sticky='nw', padx=(0, px(14)), pady=px(6))
            cell = tk.Frame(grid, bg=theme.CARD)
            cell.grid(row=row, column=1, sticky='we', pady=px(4))
            tk.Label(cell, text=title, bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9)).pack(anchor='w')
            tk.Label(cell, text=value, bg=theme.CARD, fg=color or theme.TEXT_STRONG, font=(theme.BODY_SEMI, 10),
                     justify='left', wraplength=px(620)).pack(anchor='w')
        view = self.card(body, pady=(px(14), 0))
        self.heading(view, _('How should the launcher look?'))
        ttk.Radiobutton(view, text=_('Simple (recommended): the pages you need, everything technical is set for you'),
                        variable=self.mode, value='simple', style='Card.TRadiobutton').pack(anchor='w', pady=px(2))
        ttk.Radiobutton(view, text=_('Expert: every page and every option'), variable=self.mode, value='expert',
                        style='Card.TRadiobutton').pack(anchor='w', pady=px(2))
        tk.Label(view, text=_('Switch any time at the bottom of the side bar.'), bg=theme.CARD, fg=theme.MUTED,
                 font=(theme.BODY, 9)).pack(anchor='w', pady=(px(6), 0))
        extras = self.card(body, pady=(px(14), 0))
        self.heading(extras, _('Extras', 'Дополнительно'))
        buttons = tk.Frame(extras, bg=theme.CARD)
        buttons.pack(anchor='w')
        ttk.Button(buttons, text=_('Desktop shortcut', 'Ярлык на рабочем столе'), style='Card.TButton',
                   command=app.shortcut).pack(side='left')
        ttk.Button(buttons, text=_('Add to Steam'), style='Card.TButton', command=app.add_to_steam).pack(
            side='left', padx=(px(8), 0))
        tips = tk.Frame(body, bg=theme.BG)
        tips.pack(fill='x', pady=(px(16), 0))
        for text in (_('The first start takes longer and the first minutes can stutter while the shader cache is '
                       'built. That is normal.'),
                     _('In the game, Insert (keyboard) or L3 + R3 (controller) opens the port\'s menu.'),
                     _('Next time, Play Bloodborne.exe starts the game straight away with these settings.')):
            app.state_line(tips, 'info', text, theme.BG, wrap=720).pack(fill='x', pady=px(3))

    def res_card(self, parent, res, output, recommended):
        tk, px = self.tk, self.px
        w, h = px(232), px(196)
        c = tk.Canvas(parent, width=w, height=h, bg=theme.BG, highlightthickness=0, bd=0, cursor='hand2')
        c.res, c.output, c.recommended, c.hover = res, output, recommended, False
        c.bind('<Button-1>', lambda _e: self.pick(res))
        c.bind('<Enter>', lambda _e: (setattr(c, 'hover', True), self.paint_cards()))
        c.bind('<Leave>', lambda _e: (setattr(c, 'hover', False), self.paint_cards()))
        return c

    def pick(self, res):
        self.choice = res
        self.paint_cards()

    def paint_cards(self):
        px, r, cache = self.px, self.app.rating, self.app.theme.cached
        for res, c in self.cards.items():
            w, h = int(c['width']), int(c['height'])
            on = res == self.choice
            line = theme.BLOOD_LIGHT if on else theme.BORDER_HOVER if c.hover else theme.BORDER
            c.delete('all')
            fill = theme.CARD if on else theme.SURFACE
            c.bg = cache(('res-card', w, h, line, on), lambda w=w, h=h, line=line, fill=fill, on=on: theme.rounded(
                (w, h), px(14), fill, line, (2 if on else 1) * self.app.dpi), theme.BG)
            c.create_image(0, 0, image=c.bg, anchor='nw')
            level = r.levels[res]
            c.create_text(px(20), px(36), text=res, anchor='w', fill=theme.TEXT_STRONG, font=(theme.HEAD_BOLD, 22))
            c.create_text(px(21), px(62), text=c.output.replace('x', ' × '), anchor='w', fill=theme.MUTED,
                          font=(theme.BODY, 9))
            seg_w, seg_h, gap = (w - px(40) - 4 * px(4)) / 5, px(8), px(4)
            for i in range(5):
                color = level_color(level) if i < level else theme.BORDER
                size = (round(seg_w), seg_h)
                image = cache(('seg', *size, color, fill),
                              lambda size=size, color=color: theme.rounded(size, size[1] / 2, color), fill)
                c.create_image(px(20) + round(i * (seg_w + gap)), px(88), image=image, anchor='nw')
            c.create_text(px(20), px(120), text=_(hardware.LEVELS[level]), anchor='w', fill=level_color(level),
                          font=(theme.BODY_SEMI, 13))
            c.create_text(px(20), px(146), text=hardware.settings_text(r.gpu, level, res, _) or '—', anchor='w',
                          fill=theme.TEXT_SOFT, font=(theme.BODY, 9))
            if c.recommended:
                text = _('RECOMMENDED')
                from tkinter import font as tkfont
                tw = tkfont.Font(root=self.app.root, font=(theme.HEAD, 8)).measure(text) + px(18)
                chip = cache(('chip', tw, px(22), theme.GOLD, fill), lambda tw=tw: theme.rounded(
                    (tw, px(22)), px(11), (8, 9, 10, 255), theme.GOLD, self.app.dpi), fill)
                c.create_image(px(20), h - px(26), image=chip, anchor='w')
                c.create_text(px(29), h - px(26), text=text, anchor='w', fill=theme.GOLD_LIGHT, font=(theme.HEAD, 8))
            ring = cache(('pick', on, fill), lambda on=on: theme.rounded((px(20), px(20)), px(10),
                                                                          theme.BLOOD if on else None,
                                                                          theme.BLOOD_LIGHT if on else theme.BORDER_STRONG,
                                                                          1.5 * self.app.dpi), fill)
            c.create_image(w - px(20), px(28), image=ring, anchor='e')
            if on:
                c.create_text(w - px(30), px(28), text=theme.ICON['ok'], fill=theme.TEXT_STRONG, font=(theme.ICONS, 8))

    def apply_graphics(self):
        """The settings for this PC at the chosen resolution (every performance setting, see
        hardware.profile), and frame generation when it was ticked."""
        app, r = self.app, self.app.rating
        if not r or not self.choice:
            return
        _res, _output, entries = app.recommended(self.choice)
        if self.frame_gen.get() and r.frame_gen:
            entries = [e for e in entries if e[1] != 'frame_gen'] + [('ini', 'frame_gen', r.frame_gen, '')]
        app.apply_profile(entries, remember_undo=not self.first)  # repeated: Undo on Picture & performance
