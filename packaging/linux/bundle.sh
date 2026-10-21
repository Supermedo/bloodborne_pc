#!/usr/bin/env bash
# Collects the Linux build (packaging/linux/build.sh) and what run.sh needs at run time into
# dist/bbport-linux/, one folder to copy to a Steam Deck or another PC. play.sh in it starts
# the game (BB_GAME_DIR: the game folder with eboot.bin).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/bb-probe && -f out/gpu/libbbgpu.so && -f out/gpu/libSDL3.so.0 ]] ||
    { echo 'Build first: docker compose -f packaging/linux/compose.yaml run --rm build' >&2; exit 1; }
dist=dist/bbport-linux
rm -rf "$dist" && mkdir -p "$dist/bin/gpu" "$dist/tools" "$dist/docs/evidence/TASK-001"
cp out/bb-probe out/bb-gpu-capabilities "$dist/bin/"
cp out/gpu/libbbgpu.so out/gpu/libSDL3.so.0 "$dist/bin/gpu/"
cp -r run.sh scripts patches LICENSE "$dist/"
cp tools/legacy-cpu-plan.json "$dist/tools/"
cp docs/evidence/TASK-001/allocator-span-entries.json "$dist/docs/evidence/TASK-001/"
cp packaging/linux/play.sh "$dist/"
find "$dist" -name __pycache__ -prune -exec rm -rf {} +
du -sh "$dist"
