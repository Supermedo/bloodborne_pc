# SPDX-License-Identifier: GPL-2.0-or-later
"""SuperMedo Seamless co-op's settings on the Online page: the difficulty (how much stronger enemies and
bosses get for every guest) and BBSC's main options. BBSC reads them from the game's environment at start
(bbsc/mod.json names each one's variable); the host's count for the whole session. Only settings that differ
from BBSC's own defaults are passed, so a BBSC update keeps its defaults for the rest."""
import json

L = None  # bbport_launcher_win, set by bind()


def bind(module):
    global L
    L = module


def _(*texts):
    return L._(*texts)


# BBSC's scaling, in percent per guest: (mod.json id, launcher setting).
SCALES = (('scale_enemy_hp', 'bbsc_enemy_hp'), ('scale_boss_hp', 'bbsc_boss_hp'), ('scale_damage', 'bbsc_damage'),
          ('scale_enemy_stagger', 'bbsc_enemy_poise'), ('scale_boss_stagger', 'bbsc_boss_poise'))
# Each difficulty's values for SCALES. BBSC multiplies by 1 + percent x guests (up to 4), so damage stays the
# smallest step: it falls on the one hunter a boss hunts, and NG+ adds its own. Easy is the game's own co-op for
# bosses (x1.5 with one guest, x2 with two) with no extra damage. Before 2.11.2026 Normal was BBSC's default
# (40, 100, 15, 15, 20) and the steps went up to x2.6 damage and x5 normal enemy health with four guests (Nick).
DIFFICULTIES = {
    'easy': (10, 50, 0, 5, 10),
    'normal': (30, 80, 5, 10, 20),
    'hard': (45, 110, 12, 20, 30),
    'very_hard': (60, 140, 20, 30, 40),
}
# BBSC's on/off options shown, in this order (mod.json ids); the rest keep BBSC's defaults.
OPTIONS = ('five_hunters', 'no_hp_penalty', 'full_echoes', 'guest_items', 'death_penalty', 'free_bell',
           'no_fog_walls', 'spectate', 'shared_progress', 'scale_npc_hunters')

DEFAULTS = {'bbsc_difficulty': 'normal'}
DEFAULTS.update({key: str(value) for (_id, key), value in zip(SCALES, DIFFICULTIES['normal'])})
DEFAULTS.update({f'bbsc_{option}': None for option in OPTIONS})  # None: BBSC's default (mod.json)


def mod_settings():
    """{id: setting} of bbsc/mod.json, {} when BBSC is not there or its file does not read."""
    try:
        with open(L.PORT_DIR / 'bbsc' / 'mod.json', encoding='utf-8') as file:
            return {s['id']: s for s in json.load(file).get('settings', []) if s.get('id') and s.get('env')}
    except (OSError, ValueError, AttributeError):
        return {}


def option_default(option, settings=None):
    setting = (settings if settings is not None else mod_settings()).get(option, {})
    return bool(setting.get('default', False))


def environment(s, settings=None):
    """{variable: value} for BBSC from the launcher's settings: only what differs from mod.json's defaults."""
    settings = mod_settings() if settings is None else settings
    env = {}
    # A named difficulty plays its values as they are now, not the numbers saved with it before they changed
    preset = DIFFICULTIES.get(s.get('bbsc_difficulty'))
    for index, (option_id, key) in enumerate(SCALES):
        setting = settings.get(option_id)
        if not setting:
            continue
        chosen = preset[index] if preset else s.get(key, '')
        try:
            value = max(int(setting.get('min', 0)), min(int(setting.get('max', 1000)), int(str(chosen).strip())))
        except ValueError:
            continue
        if value != setting.get('default'):
            env[setting['env']] = str(value)
    for option in OPTIONS:
        setting, chosen = settings.get(option), s.get(f'bbsc_{option}')
        if setting and chosen is not None and bool(chosen) != bool(setting.get('default', False)):
            env[setting['env']] = '1' if chosen else '0'
    return env


def build(app, f):
    """The settings' rows on the Online page (shown with the Seamless co-op choice)."""
    tk, ttk, px = app.tk, app.ttk, app.px
    settings = mod_settings()
    app.section(f, _('Seamless co-op settings'))
    # Two hosts: the party's (who made the code) and the game's (who rings the Hunt Bell, any of them).
    app.note(f, _('In the game, any of you can host: the host rings the Hunt Bell, the others ring the Kin Bell. '
                  "The host's settings below count for the whole session, not those of the party's host, who only "
                  'made the code. Changes count from the next start.'), top=0)
    # BBSC hands its kit out in the Hunter's Dream (SuperMedo's kit-in-dream patch): a new hunter gets the first
    # weapons there, and the bells rung before changed the carried items (Nick, 2026-11-02).
    app.note(f, _("A new character gets the bells in the Hunter's Dream: ring them for the first time there, at "
                  'the Doll, once you have chosen your weapon.'), top=4)

    difficulty = app.var('bbsc_difficulty', 'app')
    names = (('easy', _('Easy')), ('normal', _('Normal')), ('hard', _('Hard')), ('very_hard', _('Very hard')),
             ('custom', _('Custom')))
    choices = ttk.Frame(f)
    for value, name in names:
        ttk.Radiobutton(choices, text=name, value=value, variable=difficulty).pack(side='left', padx=(0, px(14)))
    app.row(f, _('Difficulty'), choices,
            _('How much stronger enemies and bosses get for every guest in the session.'), key='bbsc_difficulty')

    titles = (_('Enemy health per guest'), _('Boss health per guest'), _('Enemy damage per guest'),
              _('Enemy poise per guest'), _('Boss poise per guest'))
    values = ttk.Frame(f)
    setting_vars = []
    for index, ((option_id, key), title) in enumerate(zip(SCALES, titles)):
        setting = settings.get(option_id, {})
        var = app.var(key, 'app')
        setting_vars.append(var)
        ttk.Label(values, text=title).grid(row=index, column=0, sticky='w', pady=(0, px(4)))
        ttk.Spinbox(values, from_=int(setting.get('min', 0)), to=int(setting.get('max', 300)), increment=5,
                    textvariable=var, width=6).grid(row=index, column=1, sticky='w', padx=(px(12), px(4)))
        ttk.Label(values, text='%').grid(row=index, column=2, sticky='w')
    # The presets in one line; the five boxes only for Custom (they set it, too, when changed). Gridded here, not
    # with app.row: its empty title would keep the hidden boxes' row open.
    summary = app.wrap_within(ttk.Label(f, text='', justify='left', anchor='w'), 680)
    summary.grid(row=app.next_row(f), column=1, sticky='we', pady=(px(6), 0))
    values.grid(row=app.next_row(f), column=1, sticky='w', pady=(px(6), 0))
    app.index_entry(values, _('Enemy health per guest'), _('Custom'), 'bbsc_difficulty')

    def show_values(*_a):
        custom = difficulty.get() == 'custom'
        values.grid() if custom else values.grid_remove()
        now = []
        for var in setting_vars:
            try:
                now.append(int(str(var.get()).strip()))
            except ValueError:
                now.append(0)
        summary.configure(text='' if custom else _('For every guest: enemies +{0} % health, bosses +{1} % health, '
                                                    '+{2} % damage, poise +{3} % / +{4} %.').format(*now))

    applying = [False]

    def apply_difficulty(*_a):
        preset = DIFFICULTIES.get(difficulty.get())
        if preset:
            applying[0] = True
            for var, value in zip(setting_vars, preset):
                var.set(str(value))
            applying[0] = False

    def to_custom(*_a):
        if not applying[0]:
            now = []
            for var in setting_vars:
                try:
                    now.append(int(str(var.get()).strip()))
                except ValueError:
                    now.append(None)
            matching = next((name for name, preset in DIFFICULTIES.items() if tuple(now) == preset), 'custom')
            if difficulty.get() != matching:
                difficulty.set(matching)
    difficulty.trace_add('write', apply_difficulty)
    for var in setting_vars:
        var.trace_add('write', to_custom)
        var.trace_add('write', show_values)
    difficulty.trace_add('write', show_values)
    # The boxes take a named difficulty's current values: ones saved before its values changed stay behind
    apply_difficulty()
    show_values()

    labels = {'five_hunters': _('Up to 5 hunters (4 guests)'),
              'no_hp_penalty': _('Guests keep their full health'),
              'full_echoes': _('Guests get full Blood Echoes'),
              'guest_items': _('Guests pick up and keep items'),
              'death_penalty': _('A death drops a bloodstain and the echoes'),
              'free_bell': _('The Beckoning Bell costs no Insight'),
              'no_fog_walls': _('No fog walls in co-op'),
              'spectate': _('Dead hunters come back instead of leaving'),
              'shared_progress': _("Guests keep the host's bosses, doors and lamps"),
              'scale_npc_hunters': _('Hostile NPC hunters get stronger too')}
    # The options by what they are about, each group a row of its own (one list of ten read as a wall).
    groups = ((_('Together'), ('five_hunters', 'no_fog_walls', 'shared_progress')),
              (_('Guests'), ('no_hp_penalty', 'full_echoes', 'guest_items')),
              (_('Deaths'), ('death_penalty', 'spectate')),
              (_('Bells and enemies'), ('free_bell', 'scale_npc_hunters')))
    option_vars = {}
    for title, options in groups:
        boxes = ttk.Frame(f)
        for option in options:
            if option not in settings:
                continue
            chosen = app.app.get(f'bbsc_{option}')
            var = option_vars[option] = tk.BooleanVar(
                value=option_default(option, settings) if chosen is None else chosen)
            var.trace_add('write', lambda *_a, o=option, v=var: app.app.__setitem__(f'bbsc_{o}', v.get()))
            ttk.Checkbutton(boxes, text=labels[option], variable=var).pack(anchor='w', pady=(0, px(3)))
        if boxes.winfo_children():
            app.row(f, title, boxes)

    def reset():
        difficulty.set('normal')
        for option, var in option_vars.items():
            var.set(option_default(option, settings))
            app.app[f'bbsc_{option}'] = None
    app.row(f, '', ttk.Button(f, text=_('Reset to BBSC defaults'), command=reset))
    # Its characters are its own: where to find them.
    app.row(f, _('Characters'), ttk.Button(f, text=_('Seamless co-op characters…'), command=lambda: (setattr(app, 'saves_view_next', 'seamless'), app.show('saves'))),
            _('Seamless co-op plays its own characters, apart from your normal ones. Bring a normal one in on the '
              'Saves page.'))
