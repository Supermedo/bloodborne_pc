#!/usr/bin/env bash
# tools/fsr4cap/verify.sh <fsr4cap dir> [frames]: runs AMD's FSR 4.1.1 DLL (fsr4cap.exe under
# Proton, proton.sh) and the Vulkan replay (out/gpu/fsr4-bench --fsr411, assets in BB_FSR411_DIR
# or fsr4_411) on the same pseudo-random inputs for several sizes and both models, and compares
# the outputs byte by byte.
# VARIANT: int8 (default); fp8, the DLL's FP8 matrix variant (RDNA4); fp8emu, the same with FP8
# emulated through FP16 matrices on both sides (RDNA3 under GE-Proton: vkd3d-proton with
# DXIL_SPIRV_CONFIG=wmma_rdna3_workaround, the replay with the fp8emu sets).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
R=$(realpath "$1")
frames=${2:-8}
variant=${VARIANT:-int8}
source tools/fsr4cap/proton.sh
export PROTONPATH=${PROTONPATH:-$(proton_candidates | head -1)}
case $variant in
int8) ;;
fp8) export VKD3D_DISABLE_EXTENSIONS= ;;
fp8emu) export VKD3D_DISABLE_EXTENSIONS= DXIL_SPIRV_CONFIG=wmma_rdna3_workaround ;;
*) echo "VARIANT: int8, fp8 or fp8emu" >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
for c in '1280x720 1920x1080 2' '640x360 1920x1080 4' '1067x600 1600x900 2' '1707x960 2560x1440 2' \
         '2259x1271 3840x2160 2' '1280x720 3840x2160 4' '1707x720 2560x1080 2'; do
    set -- $c
    fsr4cap_run "$R" "${FSR4CAP_VERSION:-4.1.}" "$1" "$2" "$frames" noise || true
    BB_FSR411_VARIANT=$variant BENCH_NOISE=1 BENCH_DUMP=$tmp/replay.raw \
        out/gpu/fsr4-bench "$1" "$2" "$3" "$frames" --fsr411 > "$tmp/bench.log" 2>&1
    replayed=$(sed -n 's/^FSR 4.1.1: \([^ ]*\( (emulated)\)\{0,1\}\) .*/\1/p' "$tmp/bench.log" | head -1)
    if cmp -s "$R/output_$2.raw" "$tmp/replay.raw"; then
        result=bit-exact
    else
        result="DIFFERS ($( (cmp -l "$R/output_$2.raw" "$tmp/replay.raw" || true) | wc -l) bytes)"
        fail=1
    fi
    printf '%-9s <- %-9s preset %s, %s frames, replay %s: %s\n' "$2" "$1" "$3" "$frames" \
        "${replayed:-?}" "$result"
done
exit $fail
