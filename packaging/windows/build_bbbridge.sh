#!/usr/bin/env bash
# Builds bbbridge-cli.exe (bbbridge/, MaskZeus's bridge to The Hunter's Dream, GPL-3.0-or-later)
# with MSVC and puts it next to out/bb-probe.exe; package.sh ships it. The launcher and run.py
# start it for "The Hunter's Dream" co-op pool. Needs Visual Studio 2022 (Build Tools):
#   bash packaging/windows/build_bbbridge.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
cmake=${CMAKE_EXE:-cmake}
"$cmake" -S bbbridge -B out/bbbridge-build -G "Visual Studio 17 2022" -A x64
"$cmake" --build out/bbbridge-build --config Release --target bbbridge-cli
cp out/bbbridge-build/Release/bbbridge-cli.exe out/
echo "bbbridge ready: out/bbbridge-cli.exe ($(out/bbbridge-cli.exe --version))"
