# SPDX-License-Identifier: GPL-3.0-or-later
"""UI preflight for the experimental AMD driver routes.

The renderer verifies the actual 4.1.2 provider when creating its context.
fsr4vk files and its Vulkan feature contract apply only to FSR 4.1.1.
"""
import bbport_driver_runtime
from bbport_fsr411 import valid_status
from pathlib import Path
import json
import os
import subprocess

_cache = {}


def query_method(port, method, paths=None, env=None, refresh=False):
    """Check the exact driver provider on the selected GPU without creating FSR buffers."""
    port = Path(port)
    paths = bbport_driver_runtime.discover() if paths is None else paths
    api = 'vulkan' if method == 'vk' else 'dx12'
    if not paths.get(api):
        name = bbport_driver_runtime.DLLS[api]
        return dict(supported=False, reason='driver', detail=name,
                    checks=[dict(id='runtime_file', state='fail', detail=name)])
    exe = port/'bin/bb-gpu-capabilities.exe'
    if not exe.is_file(): exe = port/'out/bb-gpu-capabilities.exe'
    def stamp(path):
        try:
            value = Path(path).stat()
            return value.st_size, value.st_mtime_ns
        except OSError: return None
    environment = dict(os.environ if env is None else env)
    environment['BB_TEST_AMD_VK_RUNTIME' if method == 'vk' else 'BB_TEST_AMD_DX_RUNTIME'] = paths[api]
    key = (str(exe), stamp(exe), method, paths[api], stamp(paths[api]), environment.get('BB_GPU_ID'))
    if key in _cache and not refresh: return dict(_cache[key])
    try:
        result = subprocess.run([str(exe), '--fsr412-'+('vulkan' if method == 'vk' else 'dx12')], cwd=port, env=environment,
            capture_output=True, text=True, timeout=15,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if result.returncode:
            raise ValueError('bb-gpu-capabilities.exe: exit=' + str(result.returncode))
        response = json.loads(next(line for line in reversed(result.stdout.splitlines()) if line.startswith('{')))
        data = response.get('result', response) if isinstance(response, dict) else response
        if not valid_status(data):
            raise ValueError('invalid provider response')
        data = dict(data)
        if isinstance(response.get('gpu'), dict): data['gpu'] = response['gpu']
        data['checks'] = [dict(id='runtime_file', state='pass', detail=Path(paths[api]).name),
                          *data.get('checks', [])]
        _cache[key] = data
        return dict(data)
    except (OSError, ValueError, StopIteration, subprocess.TimeoutExpired) as error:
        detail = ('bb-gpu-capabilities.exe: timeout=15s' if isinstance(error, subprocess.TimeoutExpired)
            else str(error) or 'Invalid provider response')
        return dict(supported=False, reason='check', detail=detail,
                    checks=[dict(id='probe', state='fail', detail=detail)])


def query_vulkan(port, paths=None, env=None, refresh=False):
    return query_method(port, 'vk', paths, env, refresh)


def query_dx12(port, paths=None, env=None, refresh=False):
    return query_method(port, 'dx12', paths, env, refresh)


def method_status(method, capabilities, paths=None):
    if capabilities is None:
        return dict(supported=False, reason='checking', detail='')
    if capabilities.get('vendor_id') is None:
        return dict(supported=False, reason='check', detail='')
    if capabilities['vendor_id'] != 0x1002:
        return dict(supported=False, reason='hardware', detail='AMD GPU required')
    key = 'fsr412_vulkan' if method == 'vk' else 'fsr412_dx12'
    if key in capabilities:
        status = dict(capabilities[key])
        if method == 'dx12' and status['supported']:
            # 4.1.2 shares the bridge, not the older 4.1.1 provider requirement.
            interop = capabilities.get('dx12_interop')
            if interop is None:
                return dict(supported=False, reason='check', detail='Vulkan/DX12 sharing check missing')
            if not interop['supported']:
                return dict(interop)
        return status
    paths = bbport_driver_runtime.discover() if paths is None else paths
    api = 'vulkan' if method == 'vk' else 'dx12'
    if not paths.get(api):
        return dict(supported=False, reason='driver', detail=bbport_driver_runtime.DLLS[api])
    # DLL presence does not establish the provider version or GPU support.
    return dict(supported=False, reason='check', detail='FSR 4.1.2 provider check missing')


def message(status, translate):
    if status['supported']: return ''
    key = {'checking': 'FSR 4.1.2: checking support…',
           'hardware': 'FSR 4.1.2 requires an AMD GPU.',
           'driver': 'FSR 4.1.2: update the AMD driver.'}.get(status['reason'], 'FSR 4.1.2: update the AMD driver.')
    return translate(key)
