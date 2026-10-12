#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What an AMD FidelityFX upscaler DLL is, before a capture (build_assets.sh, the launcher).

    dll_info.py <amd_fidelityfx_upscaler_dx12.dll> [loader DLL]

Prints the DLL's file version, the FSR 4 models it holds and how it is loaded. AMD's upscaler
DLLs export the FidelityFX API themselves (ffxCreateContext, ...): fsr4cap.exe loads them
directly, no loader needed. Only a DLL without these exports is captured through the loader
(given, or found next to it). Exit status: 0 when it holds the model vk_fsr411.cpp replays
(FSR 4.1.x, fsr4_model_v07_fp8_no_scale) and can be loaded; 2 for another model or version
(community INT8 builds such as 4.0.2b are laid out differently: 16 passes, other weights; AMD's
4.0.x, e.g. Pragmata's 4.0.3, has the model but starts on RDNA4 only); 7 when it needs a loader
and no usable one was found (or an upscaler DLL was given as the loader).
"""
import re
import struct
import sys
from pathlib import Path

SUPPORTED_MODEL = "v07_fp8_no_scale"
# OptiScaler ships the FidelityFX API loader as amd_fidelityfx_dx12.dll (the same file).
LOADER_NAMES = ("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_dx12.dll")
FIXED_FILE_INFO = struct.pack("<I", 0xFEEF04BD)
# What fsr4cap.exe calls first; an upscaler DLL exporting it is loaded without the loader.
FFX_ENTRY = "ffxCreateContext"


def file_version(data):
    """The version in the DLL's VS_FIXEDFILEINFO, as a tuple of four numbers, or None."""
    at = data.find(FIXED_FILE_INFO)
    if at < 0 or at + 16 > len(data):
        return None
    ms, ls = struct.unpack_from("<II", data, at + 8)
    return (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def models(data):
    """FSR 4 models named by the DLL's shaders, e.g. {"v07_fp8_no_scale"} or {"v07_i8"}."""
    pattern = rb"fsr4_model_(v\d+(?:_[a-z0-9]+)*?)_(?:prepass|postpass|pass\d+)"
    return {m.decode() for m in re.findall(pattern, data)}


def exports(data):
    """The names a PE DLL exports; empty when it is no PE file or exports nothing."""
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return set()
        sections, = struct.unpack_from("<H", data, pe + 6)
        optional_size, = struct.unpack_from("<H", data, pe + 20)
        optional = pe + 24
        magic, = struct.unpack_from("<H", data, optional)
        rva, _size = struct.unpack_from("<II", data, optional + (112 if magic == 0x20B else 96))
        table = [struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8)
                 for i in range(sections)]

        def offset(address):  # RVA -> file offset
            for virtual_size, virtual, raw_size, raw in table:
                if virtual <= address < virtual + max(virtual_size, raw_size):
                    return raw + address - virtual
            raise ValueError(address)

        if not rva:
            return set()
        directory = offset(rva)
        count, = struct.unpack_from("<I", data, directory + 24)
        names = offset(struct.unpack_from("<I", data, directory + 32)[0])
        found = set()
        for i in range(count):
            at = offset(struct.unpack_from("<I", data, names + 4 * i)[0])
            found.add(data[at:data.index(b"\0", at)].decode("ascii", "replace"))
        return found
    except (struct.error, ValueError):
        return set()


def find_loader(upscaler):
    """The loader next to the upscaler DLL or one folder up (OptiScaler: FSR4_LATEST/), or None."""
    upscaler = Path(upscaler)
    for folder in (upscaler.parent, upscaler.parent.parent):
        for name in LOADER_NAMES:
            candidate = folder / name
            if candidate.is_file():
                return candidate
    return None


# Exit statuses (build_assets.sh passes them on; the launcher names the problem from them).
UNSUPPORTED = 2
NO_LOADER = 7


def inspect(upscaler, loader=None):
    """(status, problem or None, details): what the DLLs are and whether bbport can use them.
    details["loader"]: the loader to capture with, None when the upscaler DLL exports the API.
    details["kind"] names the problem for the launcher's messages: not_fsr4, fsr4_0 (official
    4.0.x: AMD enables it on RDNA4 only, so it cannot run here), other_model, no_loader,
    loader_is_upscaler, old_loader."""
    try:
        data = Path(upscaler).read_bytes()
    except OSError as e:
        return UNSUPPORTED, f"cannot read {upscaler}: {e.strerror}", {"kind": "not_fsr4"}
    version = file_version(data)
    found = models(data)
    details = {"version": ".".join(map(str, version)) if version else "?",
               "models": sorted(found)}
    if not found:
        details["kind"] = "not_fsr4"
        return UNSUPPORTED, "not an FSR 4 upscaler DLL (no FSR 4 model inside)", details
    if SUPPORTED_MODEL not in found:
        details["kind"] = "other_model"
        return (UNSUPPORTED, f"FSR {details['version']} with model {', '.join(sorted(found))}: "
                f"bbport replays the FSR 4.1.x model ({SUPPORTED_MODEL}) only", details)
    if version and version[:2] < (4, 1):
        # Pragmata's 4.0.3: the same model name, but its provider is only offered on RDNA4 (FP8).
        details["kind"] = "fsr4_0"
        return (UNSUPPORTED, f"FSR {details['version']}: AMD's FSR 4.0.x runs on RDNA4 only and does "
                f"not start under Proton on other GPUs; FSR 4.1.x is needed", details)
    if FFX_ENTRY in exports(data):
        details["loader"] = None
        return 0, None, details
    loader = Path(loader) if loader else find_loader(upscaler)
    if not loader or not loader.is_file():
        details["kind"] = "no_loader"
        return (NO_LOADER, "no loader DLL (" + " or ".join(LOADER_NAMES) +
                ") next to it or one folder up", details)
    details["loader"] = str(loader)
    try:
        loader_data = loader.read_bytes()
    except OSError as e:
        details["kind"] = "no_loader"
        return NO_LOADER, f"cannot read {loader}: {e.strerror}", details
    if models(loader_data):
        details["kind"] = "loader_is_upscaler"
        return (NO_LOADER, f"{loader.name} is an upscaler DLL, not the loader (" +
                " or ".join(LOADER_NAMES) + ")", details)
    loader_version = file_version(loader_data)
    details["loader_version"] = ".".join(map(str, loader_version)) if loader_version else "?"
    if not loader_version or loader_version[0] < 2:
        details["kind"] = "old_loader"
        return (NO_LOADER, f"the loader {loader.name} is version {details['loader_version']}; "
                f"FSR 4 needs the FidelityFX API loader 2.x", details)
    return 0, None, details


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: dll_info.py <amd_fidelityfx_upscaler_dx12.dll> [loader DLL]", file=sys.stderr)
        return 1
    status, problem, details = inspect(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
    print(f"Upscaler DLL: version {details.get('version', '?')}, models: "
          f"{', '.join(details.get('models', [])) or 'none'}")
    if details.get("loader"):
        print(f"Loader DLL: {details['loader']} (version {details.get('loader_version', '?')})")
    elif "loader" in details:
        print("It exports the FidelityFX API itself: no loader needed")
    if problem:
        print(f"Not usable: {problem}", file=sys.stderr)
        return status
    if details["loader"]:
        print(f"LOADER={details['loader']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
