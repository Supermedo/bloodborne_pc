# SPDX-License-Identifier: GPL-3.0-or-later
"""Locate original FSR DLLs in the installed driver of present AMD display devices.

Only Windows SetupAPI/registry metadata is read. Driver DLLs are neither copied
nor loaded here. A missing runtime leaves other upscalers available.
"""
from pathlib import Path
import ctypes
from ctypes import wintypes as w
import os
import sys

DISPLAY_CLASS = '{4d36e968-e325-11ce-bfc1-08002be10318}'
DLLS = {'vulkan': 'amdvkffx64.dll', 'dx12': 'amdxcffx64.dll'}


class DeviceInfo(ctypes.Structure):
    _fields_ = [('cbSize', w.DWORD), ('ClassGuid', ctypes.c_ubyte * 16),
                ('DevInst', w.DWORD), ('Reserved', ctypes.c_size_t)]


def driver_packages():
    """Return current (version, original-INF) records for physically present AMD GPUs."""
    if sys.platform != 'win32':
        return []
    import winreg
    setup = ctypes.WinDLL('setupapi', use_last_error=True)
    setup.SetupDiGetClassDevsW.argtypes = [ctypes.c_void_p, w.LPCWSTR, w.HWND, w.DWORD]
    setup.SetupDiGetClassDevsW.restype = w.HANDLE
    setup.SetupDiEnumDeviceInfo.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(DeviceInfo)]
    setup.SetupDiEnumDeviceInfo.restype = w.BOOL
    setup.SetupDiGetDeviceRegistryPropertyW.argtypes = [w.HANDLE, ctypes.POINTER(DeviceInfo),
        w.DWORD, ctypes.c_void_p, ctypes.c_void_p, w.DWORD, ctypes.c_void_p]
    setup.SetupDiGetDeviceRegistryPropertyW.restype = w.BOOL
    setup.SetupDiDestroyDeviceInfoList.argtypes = [w.HANDLE]
    setup.SetupDiDestroyDeviceInfoList.restype = w.BOOL
    setup.SetupGetInfDriverStoreLocationW.argtypes = [w.LPCWSTR, ctypes.c_void_p, w.LPCWSTR,
        w.LPWSTR, w.DWORD, ctypes.c_void_p]
    setup.SetupGetInfDriverStoreLocationW.restype = w.BOOL
    guid = (ctypes.c_ubyte * 16).from_buffer_copy(bytes.fromhex('68e9364d25e3ce11bfc108002be10318'))
    handle = setup.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 2)  # DIGCF_PRESENT
    if handle in (None, ctypes.c_void_p(-1).value):
        return []
    records = []
    def prop(info, key):
        buffer = ctypes.create_unicode_buffer(2048)
        if setup.SetupDiGetDeviceRegistryPropertyW(handle, ctypes.byref(info), key, None,
                                                   buffer, ctypes.sizeof(buffer), None):
            return buffer.value
        return ''
    try:
        for index in range(256):
            info = DeviceInfo()
            info.cbSize = ctypes.sizeof(info)
            if not setup.SetupDiEnumDeviceInfo(handle, index, ctypes.byref(info)):
                break
            if 'ven_1002' not in prop(info, 1).casefold():  # SPDRP_HARDWAREID
                continue
            driver_key = prop(info, 9)  # SPDRP_DRIVER
            if not driver_key.casefold().startswith(DISPLAY_CLASS + '\\'):
                continue
            try:
                key_name = 'SYSTEM\\CurrentControlSet\\Control\\Class\\' + driver_key
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_name, 0,
                                    winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
                    inf = winreg.QueryValueEx(key, 'InfPath')[0]
                    version = winreg.QueryValueEx(key, 'DriverVersion')[0]
                if Path(inf).name != inf or not inf.casefold().endswith('.inf'):
                    continue
                buffer = ctypes.create_unicode_buffer(32768)
                if setup.SetupGetInfDriverStoreLocationW(str(Path(os.environ['SystemRoot'])/'INF'/inf),
                                                         None, None, buffer, len(buffer), None):
                    records.append((tuple(int(v) for v in version.split('.')), Path(buffer.value)))
            except (OSError, ValueError, TypeError, KeyError):
                continue
    finally:
        setup.SetupDiDestroyDeviceInfoList(handle)
    return records


def locate(packages, repository):
    """Resolve only files belonging to the supplied installed-driver packages."""
    repository = Path(repository).resolve()
    result = {}
    for _version, inf in sorted(packages, key=lambda item: item[0], reverse=True):
        folder = Path(inf).resolve().parent
        if not folder.is_relative_to(repository):
            continue
        for api, name in DLLS.items():
            if api in result:
                continue
            for dll in sorted(folder.rglob(name)):
                resolved = dll.resolve()
                if resolved.is_relative_to(folder) and resolved.is_file():
                    result[api] = str(resolved)
                    break
    return result


def discover():
    try:
        repository = Path(os.environ['SystemRoot'])/'System32/DriverStore/FileRepository'
        return locate(driver_packages(), repository)
    except (OSError, ValueError, KeyError):
        return {}


def configure_environment(env, replace=False):
    """Shared initialization for GUI, --run and run.py; CLI overrides stay explicit."""
    paths = discover()
    for api, variable in (('vulkan', 'BB_TEST_AMD_VK_RUNTIME'), ('dx12', 'BB_TEST_AMD_DX_RUNTIME')):
        if replace:
            env.pop(variable, None)
        if paths.get(api):
            env.setdefault(variable, paths[api])
    return env
