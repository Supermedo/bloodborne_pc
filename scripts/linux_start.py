# SPDX-License-Identifier: GPL-2.0-or-later
"""The start steps run.py does on Windows, for run.sh on Linux, with run.py's own functions.

run.sh calls one step at a time and reads what it decided from a results file (bash builtins only: the
AppImage's PATH has no sed or grep):
    env KEY=VALUE     a variable for the game
    unset KEY         a variable from the previous start (the in-game restart inherits them)
    layer PATH        a generated layer below the mods (mods.py --layer)
    groups a,b        the patch groups this eboot.bin takes (patches.py PATCH_GROUPS)
    live 0|1          live resolution changes
    patch NAME        a patch to add (patches.py --extra)
A failed part is reported and left out: nothing here may stop the game from starting.

Steps: before-view (before mods.py makes the game view), ui-res (once live resolution is chosen), low-vram.
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT))
sys.path.insert(0, str(PORT / 'scripts'))
import run  # noqa: E402 (run.py, shipped beside run.sh: Windows calls live inside its functions)
from patches import (OUTPUT_SIZE, PATCH_GROUPS, game_app_version, group_list, hud_canvas,  # noqa: E402
                     image_groups, output_size, read_settings, ui_viewport)

run.no_console = lambda: 0  # a Windows flag for new processes

# Set once the saves were copied: the in-game restart inherits it and does not copy them again.
BACKED_UP = 'BB_SAVES_BACKED_UP'
# Seamless co-op's saves from a Windows port: never changed here (bbport_linux_online.COOP_SAVES).
COOP_SAVES = run.COOP_SAVES


class Results:
    def __init__(self):
        self.lines = []

    def add(self, kind, value):
        self.lines.append(f'{kind} {value}')

    def env(self, key, value):
        os.environ[key] = str(value)
        self.add('env', f'{key}={value}')

    def unset(self, key):
        os.environ.pop(key, None)
        self.add('unset', key)

    def write(self, path):
        Path(path).write_text(''.join(line + '\n' for line in self.lines), encoding='utf-8')


def attempt(name, work, *args):
    """work(*args), or a line in the log and None: run.py's scripts can end with sys.exit."""
    try:
        return work(*args)
    except (Exception, SystemExit) as error:
        print(f'{name}: {error}; the game starts without it', flush=True)
        return None


def game_groups(game):
    """The patch groups of this eboot.bin, as run.py chooses them (patches exist for 01.09 only)."""
    if os.environ.get('BB_FORCE_PATCHES'):
        return set(PATCH_GROUPS)
    version = game_app_version(game)
    if version not in (None, '01.09'):
        print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
        return set()
    try:
        groups = image_groups(run.eboot_image(game))
    except (OSError, ValueError, KeyError, IndexError):
        return set()  # prepare.py reports unreadable executables itself
    if not groups:
        print('Game files: eboot.bin is not the 01.09 build the community patches were made for; '
              '30 FPS, no effect patches, no ultrawide or output resolution patch')
    elif groups != set(PATCH_GROUPS):
        print(f'Game files: eboot.bin is modified, not the clean 01.09 build: its '
              f'{group_list(set(PATCH_GROUPS) - groups)} code differs, so those options are off; '
              f'{group_list(groups)} work')
    return groups


def parse_groups(text):
    """None: not known (the first step failed), nothing is held back."""
    return None if text == 'unknown' else {name for name in text.split(',') if name}


def port_layers(game, out, config, groups, results):
    """Quit to Desktop, PC Settings, Rebirth, BBSC's item names, the ultrawide HUD and the field of view: run.py's
    layers, in its order. patches.py adds the game code for the first three unless told here it has no texts."""
    def layer(path):
        results.add('layer', path)

    def off(*keys):
        for key in keys:
            results.env(key, '0')

    patched_game = game_app_version(game) in (None, '01.09') or os.environ.get('BB_FORCE_PATCHES')

    text_layer = out / 'quit_text'
    shutil.rmtree(text_layer, ignore_errors=True)
    quit_menu = run.ini_value(config, 'quit_to_desktop') != '0'
    pc_menu = run.ini_value(config, 'pc_menu') != '0'
    if (quit_menu or pc_menu) and patched_game:
        flags = ([] if quit_menu else ['--no-quit']) + (['--pc'] if pc_menu else [])
        if run.run_script('quit_menu.py', game, '--out', text_layer, *flags, optional=True):
            layer(text_layer)
        else:
            shutil.rmtree(text_layer, ignore_errors=True)
            off('BB_QUIT_TO_DESKTOP', 'BB_PC_MENU')
            print('Quit to Desktop: its menu texts could not be written; the game starts without it')

    rebirth_layer = out / 'rebirth'
    shutil.rmtree(rebirth_layer, ignore_errors=True)
    if run.ini_value(config, 'rebirth') != '0' and patched_game:
        weapons = out / 'rebirth-weapons.txt'  # what the equipped weapons need: kept by a rebirth
        results.env('BB_REBIRTH_WEAPONS', weapons)
        if run.run_script('rebirth.py', game, '--out', rebirth_layer, '--source', text_layer, '--weapons', weapons,
                          optional=True):
            layer(rebirth_layer)
        else:
            shutil.rmtree(rebirth_layer, ignore_errors=True)
            off('BB_REBIRTH')
            print("Rebirth: the Doll's script could not be written; the game starts without it")

    # A character brought from Seamless co-op on Windows shows no "?GoodsName?".
    items_layer = out / 'bbsc_items'
    shutil.rmtree(items_layer, ignore_errors=True)
    if run.run_script('bbsc_items.py', game, '--out', items_layer, optional=True):
        layer(items_layer)
    else:
        shutil.rmtree(items_layer, ignore_errors=True)

    hud_layer = out / 'hud_layout'
    shutil.rmtree(hud_layer, ignore_errors=True)
    canvas = hud_canvas(output_size(read_settings(Path(config))))
    if canvas != OUTPUT_SIZE and 'resolution' in groups:
        position = run.ini_value(config, 'hud_position') or '100'
        if run.run_script('hud_layout.py', game, '--out', hud_layer, '--canvas-width', canvas[0],
                          '--position', max(0, min(100, int(position) if position.isdigit() else 100)) / 100,
                          '--source', text_layer, optional=True):
            layer(hud_layer)
        else:
            shutil.rmtree(hud_layer, ignore_errors=True)
            print('HUD layout: its menu files could not be written; the HUD stays in the centre')

    fov_layer = out / 'fov'
    fov = run.ini_value(config, 'fov')
    if fov and fov.isdigit() and int(fov) != 43:
        if run.run_script('fov.py', game, '--out', fov_layer, '--fov', fov, optional=True):
            layer(fov_layer)
        else:
            shutil.rmtree(fov_layer, ignore_errors=True)
            print('Field of view: its camera table could not be written; the game keeps its own')
    else:
        shutil.rmtree(fov_layer, ignore_errors=True)
    return True


def trophy_popup(game, out, config, results):
    """The pop-up's texts and the game's own trophy pictures. The pictures need Pillow: without it the cup."""
    language = os.environ.get('BB_LANGUAGE') or read_settings(Path(config)).get('language')
    if language in ('10', '11'):
        results.add('patch', 'Unlock Game Region')
    try:
        results.env('BB_TROPHY_TEXT', run.write_trophy_text(out / 'trophies.tsv', language))
    except Exception as error:  # the pop-up then says "Trophy <id>"
        print(f'Trophy names for the pop-up: {error}')
    try:
        results.env('BB_TROPHY_ICONS', run.write_trophy_icons(out / 'trophy_icons', game))
    except Exception as error:  # the pop-up then shows the cup in the grade's colour
        print(f'Trophy pictures for the pop-up: {error}')


def saves_steps(user, results):
    """Read-only save files made writable, the copy before the game (BB_SAVE_BACKUPS=1, once per launch) and
    BBSC's bells in a character played normally put back (its saves copied first). A Windows port's seamless-coop
    folder is left alone: those are BBSC's characters, and the launcher says so."""
    run.writable_saves(user)
    if os.environ.get('BB_SAVE_BACKUPS') == '1' and not os.environ.get(BACKED_UP):
        run.backup_saves(user)
        results.env(BACKED_UP, '1')
    if Path(user).name == COOP_SAVES:
        return
    for save in sorted(Path(user).glob('savedata/*/*/SPRJ0005')):
        run.run_script('bbsc_items.py', '--repair', save, '--backups', Path(user) / 'bbsc-repair-backups', optional=True)


def game_running(proc=Path('/proc')):
    """Whether a bb-probe runs on this PC; yes when that cannot be told."""
    try:
        for entry in proc.iterdir():
            if entry.name.isdigit():
                try:
                    if (entry / 'comm').read_text(encoding='utf-8', errors='replace').strip() == 'bb-probe':
                        return True
                except OSError:
                    pass  # ended meanwhile
    except OSError:
        return True
    return False


def remove_stale_views(out, game, running=game_running):
    """Game views a game left behind (the launcher's forced stop, a power cut): now that the port's own layers make
    one at nearly every start, they would add up. Links only; mods.remove_overlay never touches what they point to."""
    stale = [path for path in Path(out).glob('mod-game-*') if path.is_dir() and not path.is_symlink()]
    if not stale or running():
        return
    from mods import remove_overlay
    for path in stale:
        try:
            remove_overlay(path, game)
            print(f'Mods: removed a game view an earlier game left behind: {path.name}')
        except OSError as error:
            print(f'Mods: {path} could not be removed: {error}')


def before_view(args, results):
    game, out = Path(args.game), Path(args.out)
    attempt('Game views', remove_stale_views, out, game)
    groups = attempt('Game files', game_groups, game)
    if groups is None:
        # Not known: the layers that depend on it wait, patches.py still checks the code it patches.
        groups = set()
    else:
        results.add('groups', ','.join(sorted(groups)))
    if not attempt('Menu layers', port_layers, game, out, args.config, groups, results):
        for key in ('BB_QUIT_TO_DESKTOP', 'BB_PC_MENU', 'BB_REBIRTH'):  # patches.py: no entries without their texts
            results.env(key, '0')
    attempt('Trophy pop-up', trophy_popup, game, out, args.config, results)
    attempt('Saves', saves_steps, args.user, results)


def ui_res(args, results):
    """run.py: without the resolution patches the size changes live; ultrawide and Sharp UI need the startup patch."""
    groups = parse_groups(args.groups)
    patched = groups is None or 'resolution' in groups
    live = args.live
    if args.output:
        live = '1' if live == '1' or not patched else '0'
        output = tuple(int(value) for value in args.output.split('x'))
        canvas = ui_viewport(output, read_settings(Path(args.config)))
        if patched and canvas != OUTPUT_SIZE:
            live = '0'
            results.env('BB_UI_RES', f'{canvas[0]}x{canvas[1]}')
            if hud_canvas(output) != OUTPUT_SIZE:
                print(f'Output {args.output}: ultrawide, HUD {canvas[0]}x{canvas[1]}')
            else:
                print(f'Output {args.output}: Sharp UI, menus and HUD drawn at {canvas[0]}x{canvas[1]}')
    if live == '1' or not args.output:
        results.unset('BB_UI_RES')
    results.add('live', live)


def low_vram(args, results):
    """run.py's low-VRAM profile: 4 and 6 GB discrete cards (BB_LOW_VRAM=1 forces it, 0 turns it off)."""
    vram = run.low_vram_mb(os.environ, args.caps)
    if not vram:
        return
    groups = parse_groups(args.groups)
    budget = os.environ.get('BB_GC_BUDGET_MB') or str(max(2048, vram - run.VRAM_RESERVE_MB))
    results.env('BB_GC_BUDGET_MB', budget)
    notes = [f'texture budget {budget} MiB']
    wants_live = (os.environ.get('BB_LIVE_RES') or run.ini_value(args.config, 'live_resolution')) == '1'
    if ((groups is None or 'resolution' in groups) and not args.output and not os.environ.get('BB_RENDER_RES')
            and not wants_live and run.ini_value(args.config, 'upscaler') != 'taa'):
        size = run.run_script('patches.py', '--print-preset-size', '--settings', args.config, capture=True).strip()
        if size:
            results.env('BB_RENDER_RES', size)
            results.env('BB_AUTO_RENDER_RES', '1')
            notes.append(f'scene {size} patched at start (live_resolution=1: live changes)')
            if run.physical_cores() <= 4 and 'BB_PREP_WORKERS' not in os.environ:
                results.env('BB_PREP_WORKERS', '0')
                notes.append('no draw-preparation workers')
    print(f'Low-VRAM profile ({vram} MiB card): {", ".join(notes)}; BB_LOW_VRAM=0 turns it off')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('step', choices=('before-view', 'ui-res', 'low-vram'))
    parser.add_argument('--results', required=True)
    parser.add_argument('--config', required=True)
    parser.add_argument('--game')
    parser.add_argument('--out')
    parser.add_argument('--user')
    parser.add_argument('--live', default='0')
    parser.add_argument('--output', default='')
    parser.add_argument('--groups', default='unknown')
    parser.add_argument('--caps')
    args = parser.parse_args(argv)
    results = Results()
    step = {'before-view': before_view, 'ui-res': ui_res, 'low-vram': low_vram}[args.step]
    try:
        attempt(args.step, step, args, results)
    finally:
        results.write(args.results)
    return 0


if __name__ == '__main__':
    sys.exit(main())
