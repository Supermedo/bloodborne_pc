# SPDX-License-Identifier: GPL-2.0-or-later
"""RecompLauncher entry points. No GUI, source repair or persistent setting overrides."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

OWNER = '.recomplauncher-prepared.json'
REQUIRED = ('boot.bin', 'boot-linked.bin', 'content.bin', 'patches.bin', 'link.json')


def progress(value, message):
    print(f'progress {value:.3f} {message}', flush=True)


def cache_folder(launcher):
    return Path(launcher.DATA_DIR).resolve() / 'out' / 'recomplauncher'


def validate_game(game):
    """Accept the supported title/update, without searching for or repairing another dump."""
    from prepare import sfo, system_folder
    from clean_eboot import CLEAN_IMAGE, image_hash
    if not Path(game).is_dir():
        raise ValueError(f'The game folder {game} is not there. Pick your Bloodborne game folder again.')
    game = Path(game).resolve(strict=True)
    for item in ('eboot.bin', 'sce_module/libc.prx', 'dvdroot_ps4'):
        if not (game / item).exists():
            raise ValueError(f'The game folder is incomplete: missing {item}.')
    from patches import BLOODBORNE_IDS
    metadata = sfo((system_folder(game) / 'param.sfo').read_bytes())
    if metadata.get('TITLE_ID') not in BLOODBORNE_IDS:
        raise ValueError('This is not a retail release of Bloodborne. Pick your own Bloodborne game folder.')
    # Every retail release runs the same 1.09 executable; Game of the Year dumps say APP_VER 01.00 over it.
    digest = image_hash(game / 'eboot.bin')
    if digest != CLEAN_IMAGE:
        raise ValueError('The executable is not the clean 1.09 version. Apply the 1.09 update, or restore '
                         'eboot.bin from your own dump; RecompLauncher setup does not modify or repair it.')
    return game, digest


def read_marker(path):
    try:
        marker = json.loads((path / OWNER).read_text(encoding='utf-8'))
        return marker if isinstance(marker, dict) else None
    except (OSError, ValueError):
        return None


def already_prepared(launcher, game=None):
    """This release prepared the cache (from `game`, when given) and it is complete."""
    cache = cache_folder(launcher)
    marker = read_marker(cache)
    if not owned(cache) or marker.get('version') != launcher.VERSION_TEXT:
        return False
    if game is not None and marker.get('game') != str(Path(game).resolve()):
        return False
    return all((cache / name).is_file() for name in REQUIRED)


def owned(path):
    try:
        marker = json.loads((path / OWNER).read_text(encoding='utf-8'))
        return marker.get('format') == 1 and marker.get('owner') == 'bbport-recomplauncher'
    except (OSError, ValueError, AttributeError):
        return False


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def overlaps(a, b):
    """Use filesystem identity for case-insensitive paths and directory aliases too."""
    for parent, child in ((a, b), (b, a)):
        for at in (child, *child.parents):
            if parent == at or (parent.exists() and at.exists() and parent.samefile(at)):
                return True
    return False


def destination_for(launcher, game):
    destination = cache_folder(launcher)
    if overlaps(game, destination):
        raise ValueError('The game dump and the preparation cache must be in separate folders.')
    if linked(destination) or (destination.exists() and not owned(destination)):
        raise ValueError('The preparation destination exists and is not managed by this integration.')
    return destination


def prepare(launcher, settings, game, digest):
    destination = destination_for(launcher, game)
    transaction = destination.with_name(destination.name + '.preparing')
    transaction.parent.mkdir(parents=True, exist_ok=True)
    try:
        transaction.mkdir()
    except FileExistsError:
        raise ValueError('A setup is running or an interrupted setup remains in '
                         f'{transaction}. Keep it for recovery or move it aside before retrying.') from None
    staged = transaction / 'new'
    moved_old = published = preserve = False
    try:
        # Preparing files needs no GPU discovery or first-run graphics selection.
        from patches import read_settings
        graphics = {**launcher.INI_DEFAULTS, **read_settings(launcher.ini_path())}
        env = launcher.game_environment(settings, graphics)
        env.update(BB_GAME_DIR=str(game), BB_READ_ONLY_GAME='1', BB_PREPARED_OUT=str(staged),
                   BB_CONFIG=str(launcher.ini_path().resolve()))
        progress(0.10, 'Preparing your game files')
        process = subprocess.Popen([*launcher.run_command(), '--prepare-only'],
                                   cwd=launcher.PORT_DIR, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   creationflags=launcher.NO_WINDOW)
        try:
            for raw in iter(process.stdout.readline, b''):
                print(raw.decode('utf-8', errors='replace').rstrip('\r\n'), flush=True)
            code = process.wait()
        except BaseException:
            preserve = True
            process.terminate()
            process.wait()
            # A script subprocess may still be finishing; retain its isolated workspace.
            raise
        finally:
            process.stdout.close()
        if code:
            raise ValueError(f'Game preparation failed (exit {code}). See the setup log above.')
        for name in REQUIRED:
            if not (staged / name).is_file() or not (staged / name).stat().st_size:
                raise ValueError(f'Game preparation did not produce {name}.')
        (staged / OWNER).write_text(json.dumps({'format': 1, 'owner': 'bbport-recomplauncher',
                                               'game': str(game), 'image_sha256': digest,
                                               'version': launcher.VERSION_TEXT}) + '\n',
                                    encoding='utf-8')
        if destination.exists():
            if linked(destination) or not owned(destination):
                raise ValueError('The destination changed during setup; its files were kept.')
            destination.rename(transaction / 'old')
            moved_old = True
        staged.rename(destination)
        published = True
        progress(1, 'Ready to play')
    finally:
        if moved_old and not published:
            try:
                (transaction / 'old').rename(destination)
            except OSError:
                preserve = True
                print(f'The previous preparation is preserved in {transaction / "old"}.', file=sys.stderr)
        if not preserve:
            try:
                shutil.rmtree(transaction)
            except OSError as error:
                print(f'Preparation files remain in {transaction}: {error}', file=sys.stderr)


@contextmanager
def environment(values):
    previous = {key: os.environ.get(key) for key in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def main(argv, launcher):
    parser = argparse.ArgumentParser(description=__doc__)
    roles = parser.add_mutually_exclusive_group(required=True)
    roles.add_argument('--prepare-content', dest='mode', action='store_const', const='prepare')
    roles.add_argument('--recomplauncher-play', dest='mode', action='store_const', const='play')
    # Optional for setup: RecompLauncher runs it again after each release without the player's copy
    # (keeps_copy), and the game folder recorded by the last setup is used.
    parser.add_argument('--game-dir', type=Path)
    args = parser.parse_args(argv)
    if args.mode == 'play' and args.game_dir is None:
        parser.error('--recomplauncher-play needs --game-dir')
    try:
        source = args.game_dir
        if args.mode == 'prepare':
            progress(0, 'Checking your Bloodborne copy')
            if already_prepared(launcher, source):
                progress(1, 'Already prepared')
                return 0
            if source is None:
                source = (read_marker(cache_folder(launcher)) or {}).get('game')
                if not source:
                    raise ValueError('Pick your Bloodborne game folder to set it up.')
        game, digest = validate_game(source)
        if launcher.updater.underway():
            raise ValueError('An update is being installed. Wait for it to finish before starting.')
        if launcher.game_underway():
            raise ValueError('Bloodborne is already running or being prepared. Close it before retrying.')
        saved = launcher.load_settings()
        settings = {**launcher.APP_DEFAULTS, **saved, 'game_dir': str(game)}
        settings.update(launcher.settled_online(settings, saved))
        if args.mode == 'prepare':
            prepare(launcher, settings, game, digest)
            return 0
        destination = destination_for(launcher, game)
        if not destination.exists():
            raise ValueError('Run content preparation in RecompLauncher before starting Bloodborne.')
        problem = launcher.start_problem(settings)
        if problem:
            raise ValueError(problem[0])
        with environment({'BB_PREPARED_OUT': str(destination), 'BB_READ_ONLY_GAME': '1'}):
            code = launcher.play_without_window(settings)
        if code is None:
            raise ValueError('The selected graphics configuration is unavailable. Review it in Bloodborne.')
        return code
    except (OSError, ValueError, KeyError, IndexError, struct.error, StopIteration,
            subprocess.SubprocessError) as error:
        print(str(error).replace('\r', ' ').replace('\n', ' '), file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        return 130
