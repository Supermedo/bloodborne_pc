#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Frozen console smoke check. No network, launcher window or saved settings."""
from pathlib import Path
import os
import subprocess
import sys
import traceback

PASS = 'PASS: frozen TLS context, Pillow, Tk imports and Tcl initialization'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def bundle_files(folder):
    """Native dependencies and Tcl/Tk scripts, using their paths within the bundle."""
    folder = Path(folder)
    require(folder.is_dir(), f'Runtime bundle is missing: {folder}')
    native = [p for p in folder.rglob('*') if p.is_file() and p.suffix.lower() in ('.pyd', '.dll')]
    names = {p.name.lower() for p in native}
    require('_ssl.pyd' in names, 'SSL extension missing from the check bundle')
    require(f'python{sys.version_info.major}{sys.version_info.minor}.dll' in names,
            'Python DLL missing from the check bundle')
    for prefix in ('libssl', 'libcrypto'):
        require(any(name.startswith(prefix) and name.endswith('.dll') for name in names),
                f'{prefix} DLL missing from the check bundle')
    data = [p for name in ('_tcl_data', '_tk_data') for p in (folder / name).rglob('*') if p.is_file()]
    return sorted({p.relative_to(folder) for p in native + data})


def compare_bundles(own, other):
    own, other = Path(own), Path(other)
    require(other.is_dir(), f'Launcher runtime bundle is missing: {other}')
    for relative in bundle_files(own):
        target = other / relative
        require(target.is_file(), f'Launcher runtime file missing: {relative}')
        require((own / relative).read_bytes() == target.read_bytes(),
                f'Launcher runtime file differs from the check bundle: {relative}')


def check_tcl(interpreter, bundle=None):
    require(bool(interpreter.eval('info patchlevel')), 'Tcl did not initialize')
    if bundle is None:
        return
    library = interpreter.eval('info library')
    if library.startswith('//zipfs:'):
        # Tcl 9 on Windows embeds its script library in its DLL.
        mounts = interpreter.splitlist(interpreter.call('zipfs', 'mount'))
        roots = [mounts[i + 1] for i in range(0, len(mounts) - 1, 2)
                 if library.startswith(str(mounts[i]).rstrip('/') + '/')]
        require(bool(roots), 'Cannot locate the embedded Tcl library')
        library = roots[0]
    require(Path(library).resolve().is_relative_to(Path(bundle).resolve()),
            'Tcl loaded its script library from outside the frozen bundle')


def check(against=None):
    import ssl
    import tkinter
    import tkinter.font
    from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont, ImageStat, ImageTk
    import bbport_tls

    verified = bbport_tls.tls_context(stores=[])
    require(verified.check_hostname and verified.verify_mode == ssl.CERT_REQUIRED,
            'TLS context does not require hostname and certificate checking')
    image = Image.new('RGB', (2, 2))
    ImageDraw.Draw(image).point((0, 0), fill='white')
    image = ImageEnhance.Contrast(image).enhance(1).filter(ImageFilter.BLUR)
    require(len(ImageStat.Stat(image).mean) == 3, 'Pillow image operations failed')
    require(ImageFont.load_default(size=12).getbbox('Tk')[2] > 0, 'Pillow font rendering failed')
    require(callable(ImageTk.PhotoImage), 'Pillow Tk bridge missing')
    own = Path(sys._MEIPASS) if getattr(sys, 'frozen', False) else None
    check_tcl(tkinter.Tcl(), own)
    if own is not None:
        bundle_files(own)
    if against is not None:
        require(own is not None, 'Bundle comparison requires a frozen check program')
        compare_bundles(own, against)
    print(PASS, flush=True)


def check_launcher(executable, script, timeout=30):
    """Also exercise Python modules and data in the actual windowed bundle."""
    executable, script = Path(executable).resolve(), Path(script).resolve()
    require(executable.is_file(), 'Launcher executable missing')
    require(script.is_file(), 'Runtime check script missing')
    check(executable.parent / '_internal')
    env = os.environ.copy()
    if sys.platform == 'win32':
        # The launcher must work without MSYS2 or the build PC's Python on PATH.
        windows = Path(os.environ['SystemRoot'])
        env['PATH'] = os.pathsep.join((str(windows / 'System32'), str(windows)))
    try:
        result = subprocess.run([str(executable), '--script', str(script)], cwd=executable.parent,
                                capture_output=True, encoding='utf-8', errors='replace', env=env,
                                timeout=timeout, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except subprocess.TimeoutExpired as exc:
        output = ''.join(part.decode('utf-8', errors='replace') if isinstance(part, bytes) else part or ''
                         for part in (exc.stdout, exc.stderr))
        raise RuntimeError(f'Actual launcher runtime check timed out after {timeout} seconds. '
                           'An error dialog may be blocking startup; '
                           'rerun the command manually to inspect it.'
                           f'\nCaptured output:\n{output}') from exc
    require(result.returncode == 0,
            f'Actual launcher runtime check failed ({result.returncode}): {result.stdout}{result.stderr}')
    require(PASS in result.stdout.splitlines(), 'Actual launcher did not complete its runtime check')
    print('PASS: actual windowed launcher runtime', flush=True)


def main(argv=None):
    try:
        args = sys.argv[1:] if argv is None else argv
        if args and args[0] == '--launcher':
            require(len(args) == 3, 'Usage: --launcher EXE CHECK_SCRIPT')
            check_launcher(args[1], args[2])
        else:
            require(len(args) <= 1, 'Usage: check_launcher_runtime [BUNDLE]')
            check(args[0] if args else None)
    except Exception:
        traceback.print_exc()
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
