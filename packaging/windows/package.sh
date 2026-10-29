#!/usr/bin/env bash
# Builds dist/bbport-windows/ (and dist/bbport-windows.zip): Bloodborne.exe (the launcher, frozen
# with PyInstaller so players need no Python), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs,
# the preparation scripts and run.py. Run from an MSYS2 CLANG64 shell after `bash build.sh`.
# Freezing uses a Windows Python 3.10+ (python.org; WINPYTHON overrides) and a private venv in
# out/pyenv with PyInstaller. Includes the pinned fsr4vk provider and licenses, plus a
# corresponding-source ZIP beside the Windows ZIP. FSR 4 assets in fsr4_shaders/ are included when present.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
zip_only=0
if [[ ${1:-} == --zip-only ]]; then
    zip_only=1
elif [[ $# != 0 ]]; then
    echo 'Usage: bash packaging/windows/package.sh [--zip-only]' >&2
    exit 1
fi
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe && -f out/bb-fg401-capabilities.exe && -f out/bb-play.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

# A Windows Python (not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
# Windows Python needs USERPROFILE (some MSYS2 shells start without it).
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
# Pillow draws the launcher's look (launcher/bbport_theme.py) and scales the cover art.
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller pillow
# The scripts run inside Bloodborne.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 bisect collections contextlib hashlib json re shutil struct tempfile \
              xml.etree.ElementTree urllib.request ctypes.wintypes; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Bloodborne --icon "$(cygpath -w "$PWD/launcher/bloodborne.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    --add-data "$(cygpath -w "$PWD/launcher/assets");assets" \
    "$(cygpath -w "$PWD/launcher/bbport_launcher_win.py")"

# The package is assembled in a fresh staging folder and zipped from there; dist/bbport-windows
# (a playable copy that may hold saves and settings) is only refreshed afterwards.
dest=out/stage/bbport-windows
rm -rf -- out/stage
mkdir -p "$dest/bin" "$dest/launcher"
cp -r out/pyi-dist/Bloodborne/. "$dest/"
llvm-strip -o "$dest/Play Bloodborne.exe" out/bb-play.exe
cp launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe bb-fg401-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, ...). Not the Vulkan loader:
# the game uses the one the graphics driver installs in System32, as every release so far. On a PC
# with a graphics driver ldd finds that one; a build machine without one (CI) would ship CLANG64's.
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" "$dest/bin/bb-fg401-capabilities.exe" |
    awk '/\/clang64\/bin\// && !/vulkan-1\.dll/ {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
# FSR 4.1.1: the original AMD DX12 runtime, loaded dynamically beside bb-probe.exe.
# The first build downloads AMD's SDK archive; subsequent builds verify the cached DLL.
out/pyenv/Scripts/python.exe packaging/windows/fetch_fsr411_dx12.py
cp out/fsr411-dx12/amd_fidelityfx_upscaler_dx12.dll "$dest/bin/"
out/pyenv/Scripts/python.exe packaging/windows/fetch_fg401_dx12.py
cp out/fsr411-dx12/amd_fidelityfx_framegeneration_dx12.dll \
   out/fsr411-dx12/amd_fidelityfx_loader_dx12.dll out/fsr411-dx12/D3D12Core.dll "$dest/bin/"
# Vulkan FSR 4.1.1: share the launcher's pinned provider and file checks. Cache the
# provider and its corresponding source so subsequent package builds work offline.
out/pyenv/Scripts/python.exe packaging/windows/prepare_fsr4vk.py --stage "$dest/fsr4vk"
# Legacy CPU (Intel 3rd generation and older): the translation plan scripts/cpu_compat.py applies.
mkdir -p "$dest/tools" && cp tools/legacy-cpu-plan.json "$dest/tools/"
cp run.py LICENSE README.md packaging/windows/README-Windows.txt "$dest/"
# The BB Report Tool the Discord helpers ask for: Help > Save a report runs it; a double-click does too.
cp packaging/windows/BB-Report-Tool.bat "$dest/"
if [[ -d fsr4_shaders ]]; then cp -r fsr4_shaders "$dest/"; fi
# DLSS (NVIDIA RTX): the MSVC-built bridge and NVIDIA's runtime, next to bb-probe.exe
# (packaging/windows/build_dlss.sh). Without them the DLSS option stays unavailable.
if [[ -f out/bbport_dlss.dll && -f out/nvngx_dlss.dll ]]; then
    cp out/bbport_dlss.dll out/nvngx_dlss.dll "$dest/bin/"
    if [[ -f out/nvngx_dlssg.dll ]]; then cp out/nvngx_dlssg.dll "$dest/bin/"; fi
    mkdir -p "$dest/licenses" && cp out/NVIDIA-DLSS-LICENSE.txt "$dest/licenses/"
    cp gpu/dlss_bridge/LICENSE.txt "$dest/licenses/bbport_dlss-LICENSE.txt"
else
    echo "DLSS bridge not built (packaging/windows/build_dlss.sh): no DLSS in this package" >&2
fi
# Online co-op: the libraries compiled in (wepoll, cpp-httplib) and the MSYS2 DLLs it loads.
mkdir -p "$dest/licenses"
cp gpu/third_party/wepoll/LICENSE "$dest/licenses/wepoll-LICENSE.txt"
cp gpu/third_party/httplib/LICENSE "$dest/licenses/cpp-httplib-LICENSE.txt"
cp packaging/windows/licenses/*.txt "$dest/licenses/"
cp packaging/windows/licenses/AMD-FSR-SDK-*.md "$dest/licenses/"
# Setup and provider requirements must be available in the downloaded package too.
mkdir -p "$dest/docs"
cp docs/FSR41_WINDOWS.md docs/FSR411_WINDOWS.md docs/OBJECT_MOTION_CACHE.md docs/ML_FRAME_GENERATION_WINDOWS.md "$dest/docs/"
mkdir -p "$dest/docs/benchmarks"
cp docs/benchmarks/fsr411-vulkan-20261016.json "$dest/docs/benchmarks/"
cp docs/benchmarks/object-motion-cache-20261016.json docs/benchmarks/fsr-sewer-20261016.json "$dest/docs/benchmarks/"
cp docs/benchmarks/fsr411-sharpen-warehouse-20261020.json "$dest/docs/benchmarks/"
cp docs/benchmarks/ml-fg401-20261017.json "$dest/docs/benchmarks/"
cp docs/benchmarks/mlfg-sharpen-warehouse-20261020.json "$dest/docs/benchmarks/"
cp docs/benchmarks/ml-fg401-profile.ini "$dest/docs/benchmarks/"
cp launcher/assets/fonts/Cinzel-OFL.txt "$dest/licenses/Cinzel-OFL.txt"  # the launcher's fonts
cp launcher/assets/fonts/Inter-LICENSE.txt "$dest/licenses/Inter-OFL.txt"
# Where the online code comes from (shadPS4 0.20, Awaken's port, bbhost's protocol).
cp gpu/bbnet/SHADNET-PORT.md "$dest/licenses/online-code-NOTICE.md"
for package in protobuf abseil-cpp openssl miniupnpc zlib; do
    license=/clang64/share/licenses/$package/LICENSE
    if [[ -f $license ]]; then cp "$license" "$dest/licenses/$package-LICENSE.txt"; fi
done
find "$dest" -name __pycache__ -prune -exec rm -r {} +
# The program files of this version (bbport-files.json): run.py and Help find mixed versions with it.
out/pyenv/Scripts/python.exe scripts/package_files.py --write "$dest" "$(sed -n "s/^VERSION = '\(.*\)'$/\1/p" launcher/bbport_launcher_win.py)"
mkdir -p dist
# Publish this source archive alongside the Windows ZIP; SOURCE.txt names both the
# companion archive and its original download. Keep it outside the playable folder.
cp out/fsr4vk/fsr4vk-v0.4.3-amdfix-src.zip dist/
rm -f dist/bbport-windows.zip
# Use the same Python as the frozen launcher build; no PowerShell Archive module
# dependency (and no Compress-Archive 2 GiB per-file limit).
out/pyenv/Scripts/python.exe -c \
    "import shutil; shutil.make_archive('dist/bbport-windows', 'zip', 'out/stage')"

# Build/CI validation can create the ZIP without touching a playable copy.
if [[ $zip_only == 1 ]]; then
    du -sh "$dest" dist/bbport-windows.zip
    exit 0
fi

# Refresh dist/bbport-windows, keeping what players create there (saves, settings, mods), and
# only while nothing runs from it: deleting a running launcher's files breaks it.
play=dist/bbport-windows
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '$(cygpath -w "$PWD/$play")\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "dist/bbport-windows is in use ($running processes): not refreshed; the zip is ready." >&2
else
    mkdir -p "$play"
    find "$play" -mindepth 1 -maxdepth 1 ! -name user ! -name mods ! -name bbport.ini \
        ! -name fsr4vk ! -name input.ini ! -name mods.json ! -name patches.json ! -name last_run.log -exec rm -rf {} +
    cp -r "$dest/." "$play/"
fi
du -sh "$dest" dist/bbport-windows.zip
