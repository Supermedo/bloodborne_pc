#!/usr/bin/env bash
# tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> [loader DLL 2.3.x]
#
# Builds the FSR 4.1.1 asset set (fsr4_411/: SPIR-V of every pass, model weights) from AMD's
# upscaler DLL, for upscaler=fsr411 (gpu/.../fsr411). Nothing of AMD's is downloaded or
# shipped: the DLLs are the user's own (OptiScaler ships them, many games do). The launcher runs
# it from its "Build FSR 4.1.1 from an AMD DLL" button.
#   0. dll_info.py checks the DLL (FSR 4.1.x with the model vk_fsr411.cpp replays). AMD's DLL
#      exports the FidelityFX API itself and fsr4cap.exe loads it directly; only a DLL that does
#      not needs the loader (given, or amd_fidelityfx_loader_dx12.dll / OptiScaler's
#      amd_fidelityfx_dx12.dll next to it or one folder up);
#   1. builds dxil-spirv (pinned commit + dxil-spirv-class-bindings.patch) and fsr4cap.exe
#      (MinGW), unless BB_FSR4CAP_TOOLS names a folder holding both (the AppImage);
#   2. runs fsr4cap.exe under Proton for every output size class and quality ratio, recording the
#      D3D12 frames (capture_all.sh; proton.sh: umu-run, or Steam's runtime of that Proton);
#   3. extract.py translates them, checks the replay rules and writes the set; it replaces the
#      previous one only once complete;
#   4. with VERIFY=1, verify.sh compares the replay with the DLL byte by byte.
# BB_FSR411_OUT: where the set goes (default fsr4_411 in the tree); BB_FSR4CAP_WORK: the work
# folder (default out/fsr4cap; it must be under $HOME for Steam's runtime); BB_FSR4CAP_CLEAN=1:
# the work folder (Proton prefix ~300 MB, captures) is removed after a successful build.
# Without BB_FSR4CAP_TOOLS it needs x86_64-w64-mingw32-gcc, cmake, ninja, python3,
# spirv-dis/spirv-as, git (or nix-shell) and network for dxil-spirv; with it, python3 and
# spirv-dis/spirv-as only. Always: a Proton build that starts the DLL's FSR 4.1 (PROTONPATH, or
# the first that does of GE-Proton 10+, Proton-CachyOS, Proton - Experimental: capture_all.sh).
# Exit status: 2 the DLL is not supported, 3 no Proton or runtime, 4 tools missing, 5 the
# upscaler did not run under Proton, 6 the captures break the replay rules, 7 the DLL needs a
# loader (it does not export the FidelityFX API) and none was found,
# 8 NixOS without umu-run (the AppImage cannot start Steam's runtime there).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
cd -- "$root"
upscaler=$(realpath "${1:?upscaler DLL}")
loader=${2:+$(realpath "$2")}
work=$(realpath -m "${BB_FSR4CAP_WORK:-$root/out/fsr4cap}")
out=$(realpath -m "${BB_FSR411_OUT:-$root/fsr4_411}")
tools=${BB_FSR4CAP_TOOLS:-}
python=${PYTHON:-python3}

# Before anything long: is it a DLL bbport can replay, and where is its loader? (Without Python
# yet, after nix-shell below.)
check_dll() {
    local status=0 info
    info=$("$python" "$here/dll_info.py" "$upscaler" ${loader:+"$loader"}) || status=$?
    echo "$info" | grep -v '^LOADER='
    [[ $status == 0 ]] || exit "$status"
    loader=${loader:-$(echo "$info" | sed -n 's/^LOADER=//p')}
    export BB_FSR4CAP_CHECKED=1
}
if [[ -z ${BB_FSR4CAP_CHECKED:-} ]] && command -v "$python" >/dev/null; then
    check_dll
fi

# The tools from PATH when all are there; else from nix-shell. Nix installed on another
# distribution often has no nixpkgs channel ("file 'nixpkgs' was not found", issue #22): then
# nixpkgs comes from the nixos-unstable channel URL.
# Python: the one named by PYTHON (the package sets it; python3 is not on its PATH) or python3.
needed=("$python" spirv-dis spirv-as)
[[ -n $tools ]] || needed+=(x86_64-w64-mingw32-gcc cmake ninja gcc git)
missing=()
for tool in "${needed[@]}"; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
if [[ ${#missing[@]} -ne 0 && -z ${BB_FSR4CAP_SHELL:-} ]] && command -v nix-shell >/dev/null; then
    nixpkgs=()
    nix-instantiate --find-file nixpkgs >/dev/null 2>&1 || nixpkgs=(-I nixpkgs=channel:nixos-unstable)
    exec env BB_FSR4CAP_SHELL=1 nix-shell "${nixpkgs[@]}" -p pkgsCross.mingwW64.buildPackages.gcc cmake \
        ninja gcc python3 spirv-tools git umu-launcher \
        --run "bash $(printf %q "$0") $(printf %q "$upscaler") ${loader:+$(printf %q "$loader")}"
fi
if [[ ${#missing[@]} -ne 0 ]]; then
    echo "Missing tools: ${missing[*]}. Install them (or Nix), then run this again:" >&2
    echo "  Arch, CachyOS: sudo pacman -S mingw-w64-gcc cmake ninja python spirv-tools git" >&2
    echo "  Fedora, Bazzite: sudo dnf install mingw64-gcc cmake ninja-build python3 spirv-tools git" >&2
    echo "  Debian, Ubuntu: sudo apt install gcc-mingw-w64-x86-64 cmake ninja-build python3 spirv-tools git" >&2
    echo "(umu-launcher is optional: without it Proton runs in Steam's own runtime.)" >&2
    exit 4
fi
if [[ -z ${BB_FSR4CAP_CHECKED:-} ]]; then
    check_dll
fi
mkdir -p "$work"
# copy_in <file> <destination>: also over a read-only copy from an earlier build (the package's
# files are read-only, and so are their copies: "cp: cannot create regular file").
copy_in() {
    rm -f -- "$2"
    cp -- "$1" "$2"
    chmod u+w -- "$2"
}

if [[ -n $tools ]]; then
    dxil_spirv=$tools/dxil-spirv
    copy_in "$tools/fsr4cap.exe" "$work/fsr4cap.exe"
else
    # dxil-spirv: DXIL -> SPIR-V as vkd3d-proton translates it.
    dx=$work/dxil-spirv
    # A build that no longer starts (127: its library path names the tree where it was built,
    # before the tree moved) is built again.
    status=0
    "$dx/build/dxil-spirv" --help >/dev/null 2>&1 || status=$?
    if [[ ! -x $dx/build/dxil-spirv || $status == 126 || $status == 127 ]]; then
        rm -rf "$dx"
        git clone -q https://github.com/HansKristian-Work/dxil-spirv.git "$dx"
        git -C "$dx" checkout -q 7dc52786cdb1f53c54dd5c5c2698dff00ea5f0a3
        git -C "$dx" submodule update -q --init --recursive
        git -C "$dx" apply "$here/dxil-spirv-class-bindings.patch"
        cmake -S "$dx" -B "$dx/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
        ninja -C "$dx/build" dxil-spirv >/dev/null
    fi
    dxil_spirv=$dx/build/dxil-spirv
    # The FidelityFX API headers (MIT) are in tools/fsr4cap/ffx.
    x86_64-w64-mingw32-gcc -std=c11 -O1 -Wall -I"$here/ffx/api/include" -I"$here/ffx/upscalers/include" \
        "$here/fsr4cap.c" "$here/capture.c" "$here/rootsig.c" \
        -o "$work/fsr4cap.exe" -ld3d12 -ldxguid -static
fi
copy_in "$upscaler" "$work/amd_fidelityfx_upscaler_dx12.dll"
rm -f -- "$work/amd_fidelityfx_loader_dx12.dll"
if [[ -n $loader ]]; then
    copy_in "$loader" "$work/amd_fidelityfx_loader_dx12.dll"
fi

echo "Recording the DLL's passes under Proton (20 sizes; 40 with the FP8 variant on RDNA4)..."
bash "$here/capture_all.sh" "$work"
echo "Translating the passes to SPIR-V..."
rm -rf "$out.new"
"$python" "$here/extract.py" "$dxil_spirv" "$work" "$out.new"
if [[ ${VERIFY:-0} == 1 ]]; then
    rm -rf "$out.old"
    [[ ! -e $out ]] || mv "$out" "$out.old"
    mv "$out.new" "$out"
    bash build.sh
    ninja -C out/gpu fsr4-bench >/dev/null
    bash "$here/verify.sh" "$work"
    rm -rf "$out.old"
else
    rm -rf "$out.old"
    [[ ! -e $out ]] || mv "$out" "$out.old"
    mv "$out.new" "$out"
    rm -rf "$out.old"
fi
if [[ ${BB_FSR4CAP_CLEAN:-0} == 1 ]]; then
    rm -rf "$work"
fi
echo "FSR 4.1.1 assets in $out (bbport.ini: upscaler=fsr411)"
