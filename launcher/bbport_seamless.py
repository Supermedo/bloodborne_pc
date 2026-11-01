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
# Each difficulty's values for SCALES; 'normal' is BBSC's own default.
DIFFICULTIES = {
    'easy': (20, 50, 5, 5, 10),
    'normal': (40, 100, 15, 15, 20),
    'hard': (70, 150, 25, 25, 30),
    'very_hard': (100, 200, 40, 40, 50),
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
    for option_id, key in SCALES:
        setting = settings.get(option_id)
        if not setting:
            continue
        try:
            value = max(int(setting.get('min', 0)), min(int(setting.get('max', 1000)), int(str(s.get(key, '')).strip())))
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
    app.note(f, _("The host's settings count for the whole session. Changes count from the next start."), top=0)

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
    app.row(f, '', values)

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
    boxes = ttk.Frame(f)
    option_vars = {}
    for index, option in enumerate(OPTIONS):
        if option not in settings:
            continue
        chosen = app.app.get(f'bbsc_{option}')
        var = option_vars[option] = tk.BooleanVar(value=option_default(option, settings) if chosen is None else chosen)
        var.trace_add('write', lambda *_a, o=option, v=var: app.app.__setitem__(f'bbsc_{o}', v.get()))
        ttk.Checkbutton(boxes, text=labels[option], variable=var).grid(
            row=index // 2, column=index % 2, sticky='w', padx=(0, px(24)), pady=(0, px(4)))
    app.row(f, _('Options'), boxes)

    def reset():
        difficulty.set('normal')
        for option, var in option_vars.items():
            var.set(option_default(option, settings))
            app.app[f'bbsc_{option}'] = None
    app.row(f, '', ttk.Button(f, text=_('Reset to BBSC defaults'), command=reset))
