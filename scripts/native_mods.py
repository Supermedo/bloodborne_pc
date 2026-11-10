#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate enabled native mods and write the launch-only DLL list.

Native modules are explicitly trusted code. The manifest checks prevent stale,
mispackaged or incompatible modules from being loaded accidentally; they are
not a process sandbox.
"""
import hashlib
import json
import os
from pathlib import Path
import re

from mods import native_manifest, selected

ENVIRONMENT_NAME = re.compile(r'^BB_[A-Z0-9_]{1,63}$')
SHA256 = re.compile(r'^[0-9a-f]{64}$')


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        while block := source.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def contained(root, value, description, must_exist=True):
    if not isinstance(value, str) or not value:
        raise ValueError(f'{description} must be a relative path')
    relative = Path(value)
    if relative.is_absolute() or any(part in ('', '.', '..') for part in relative.parts):
        raise ValueError(f'{description} escapes the mod folder')
    root = Path(root).resolve()
    result = (root / relative).resolve()
    try:
        result.relative_to(root)
    except ValueError as error:
        raise ValueError(f'{description} escapes the mod folder') from error
    if must_exist and (not result.exists() or result.is_symlink()):
        raise ValueError(f'{description} is missing: {value}')
    return result


def environment(root, manifest):
    values = manifest.get('environment', {})
    if not isinstance(values, dict) or len(values) > 32:
        raise ValueError('environment must be an object with at most 32 entries')
    result = {}
    for name, specification in values.items():
        if not isinstance(name, str) or not ENVIRONMENT_NAME.fullmatch(name):
            raise ValueError(f'invalid environment name: {name}')
        if isinstance(specification, str):
            if '\0' in specification or len(specification) > 4096:
                raise ValueError(f'invalid environment value: {name}')
            value = specification
        elif isinstance(specification, dict) and set(specification) == {'path'}:
            value = str(contained(root, specification['path'], name))
        else:
            raise ValueError(f'{name} must be a string or a relative path object')
        result[name] = value
    return result


def validate(root, manifest, game_sha256):
    supported = manifest.get('game_executables')
    if (not isinstance(supported, list) or not supported or
            not all(isinstance(value, str) and SHA256.fullmatch(value) for value in supported)):
        raise ValueError('game_executables must contain SHA-256 fingerprints')
    if game_sha256 not in supported:
        raise ValueError(f'unsupported game executable {game_sha256 or "unknown"}')
    integrity = manifest.get('integrity', {})
    if not isinstance(integrity, dict) or len(integrity) > 256:
        raise ValueError('integrity must be an object with at most 256 files')
    for relative, expected in integrity.items():
        if not isinstance(expected, str) or not SHA256.fullmatch(expected):
            raise ValueError(f'invalid integrity hash for {relative}')
        path = contained(root, relative, f'integrity file {relative}')
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f'integrity check failed: {relative}')
    module = contained(root, manifest['module'], 'module')
    if not module.is_file():
        raise ValueError('module is not a regular file')
    return module, environment(root, manifest)


def prepare(root, config, game_sha256, out, env, enabled=True):
    """Prepare selected modules. Returns the config path, or None.

    Environment variables declared by every discovered native manifest are
    cleared first so a game restart cannot keep a module that was disabled.
    """
    root, out = Path(root), Path(out)
    manifests = {}
    if root.is_dir():
        for folder in root.iterdir():
            if not folder.is_dir() or folder.name.startswith('.'):
                continue
            try:
                manifest = native_manifest(folder)
                if manifest:
                    manifests[folder.name] = (folder, manifest)
                    for name in manifest.get('environment', {}):
                        if isinstance(name, str) and ENVIRONMENT_NAME.fullmatch(name):
                            env.pop(name, None)
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                pass
    env.pop('BB_NATIVE_MODS_FILE', None)
    target = out / 'native-mods.json'
    if not enabled:
        target.unlink(missing_ok=True)
        return None
    entries, launch_environment = [], {}
    for name in selected(root, config):
        if name not in manifests:
            continue
        folder, manifest = manifests[name]
        try:
            module, values = validate(folder, manifest, game_sha256)
            overlap = set(values) & set(launch_environment)
            if overlap:
                raise ValueError(f'environment conflicts with another mod: {", ".join(sorted(overlap))}')
            launch_environment.update(values)
            entries.append({'id': manifest['id'], 'module': str(module),
                            'directory': str(folder.resolve())})
        except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
            print(f'Native mod {name}: rejected: {error}')
    if not entries:
        target.unlink(missing_ok=True)
        return None
    out.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix('.json.tmp')
    temporary.write_text(json.dumps({'schema': 1, 'mods': entries}, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, target)
    env.update(launch_environment)
    env['BB_NATIVE_MODS_FILE'] = str(target.resolve())
    print('Native mods: ' + ', '.join(entry['id'] for entry in entries))
    return target
