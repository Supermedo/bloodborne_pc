#!/usr/bin/env bash
# tools/fsr4cap/capture_all.sh <dir with fsr4cap.exe and the upscaler DLL>
# Records FSR 4 (the DLL's 4.1.x version; FSR4CAP_VERSION, a substring of the version name) for
# output 1080p/1440p/2160p with the five quality ratios, plus other output sizes (Steam Deck,
# 1600x900, ultrawide), under Proton (proton.sh: umu-run, or Steam's runtime of that Proton).
# The first capture picks the Proton build: the first of proton_candidates (PROTONPATH, or the
# installed GE-Proton 10+, Proton-CachyOS, Proton - Experimental, Proton 11.0/10.0) under which
# the DLL starts FSR 4.1.
# Two variants of the passes (capture_<render>_<output>/ and fp8/capture_<render>_<output>/):
# INT8, which every GPU runs (vkd3d-proton's cooperative matrices hidden: proton.sh), and the
# DLL's FP8 matrix variant for RDNA4 (other passes and weights, issue #12). The FP8 one is probed
# with the first size and recorded only when the DLL dispatches other shaders with the matrices
# visible: on RDNA4. BB_FSR4CAP_FP8=1: also elsewhere, through GE-Proton's FP16 emulation of FP8
# matrices (RDNA3: DXIL_SPIRV_CONFIG=wmma_rdna3_workaround), to test the variant; 0: never.
# Without it the INT8 set is built all the same.
# Exit status 3 when no Proton build is installed (or none has its Steam runtime), 5 when the
# upscaler did not run, 8 on NixOS with neither umu-run nor nix-shell to get it (nor, in the
# AppImage, the user's systemd to run the recording on the host).
set -euo pipefail
R=$(realpath "$1")
source "$(dirname -- "$0")/proton.sh"
version=${FSR4CAP_VERSION:-4.1.}
total=20
done_count=0
# NixOS has no /lib64 loader for Steam's runtime outside Steam's FHS environment: umu-run (its own
# FHS environment) from nix-shell instead. The AppImage hides the host's /nix behind its own store
# (nix-shell and umu-run included): there the recording runs on the host, started through the
# user's systemd (systemd-run --user; the work folder, under $HOME, is seen from both sides), and
# the AppImage's own tools translate the passes afterwards.
# BB_FSR4CAP_RUNNER=steam: Steam's runtime anyway (inside an FHS environment such as steam-run).
if [[ -e /etc/NIXOS && ${BB_FSR4CAP_RUNNER:-} != steam ]] && ! command -v umu-run >/dev/null; then
    if command -v nix-shell >/dev/null && [[ -z ${BB_FSR4CAP_UMU_SHELL:-} ]]; then
        nixpkgs=()
        nix-instantiate --find-file nixpkgs >/dev/null 2>&1 || nixpkgs=(-I nixpkgs=channel:nixos-unstable)
        echo "NixOS: umu-launcher from nix-shell (the first time it is downloaded, ~1.7 GB)"
        exec env BB_FSR4CAP_UMU_SHELL=1 nix-shell "${nixpkgs[@]}" -p umu-launcher \
            --run "bash $(printf %q "$0") $(printf %q "$R")"
    fi
    if [[ -z ${BB_FSR4CAP_ON_HOST:-} ]] && command -v systemd-run >/dev/null &&
       systemctl --user show-environment >/dev/null 2>&1; then
        echo "NixOS: recording on the host (systemd-run --user), outside the AppImage"
        host=$R/host
        rm -rf "$host"
        mkdir -p "$host"
        cp -- "$(dirname -- "$0")/capture_all.sh" "$(dirname -- "$0")/proton.sh" "$host/"
        unit=bbport-fsr4cap-$$
        settings=()
        for name in PROTONPATH FSR4CAP_VERSION BB_FSR4CAP_FP8 VKD3D_DISABLE_EXTENSIONS WINEDEBUG; do
            if [[ -n ${!name+set} ]]; then settings+=("--setenv=$name=${!name}"); fi
        done
        # A login shell: the user's PATH (nix-shell) and NIX_PATH.
        systemd-run --user --unit="$unit" --collect --wait --pipe --quiet \
            --setenv=BB_FSR4CAP_ON_HOST=1 "${settings[@]}" \
            /bin/sh -lc 'exec bash "$0" "$1"' "$host/capture_all.sh" "$R" &
        relay=$!
        # Cancelled (the launcher's button): the unit on the host stops too.
        trap 'systemctl --user stop "$unit" 2>/dev/null; kill "$relay" 2>/dev/null' TERM INT
        status=0
        wait "$relay" || status=$?
        rm -rf "$host"
        exit "$status"
    fi
    echo "On NixOS the recording needs umu-launcher: install it, or Nix's nix-shell to fetch it" \
         "(and, in the AppImage, the user's systemd to start it on the host)." >&2
    exit 8
fi
mapfile -t candidates < <(proton_candidates)
if [[ ${#candidates[@]} -eq 0 ]]; then
    echo "No Proton build found: install GE-Proton 10 or newer (ProtonUp-Qt), or Proton -" \
         "Experimental or Proton-CachyOS in Steam, or set PROTONPATH to a Proton folder." >&2
    exit 3
fi

# capture <render WxH> <output WxH>: 0 when the DLL ran its frames; 3 without a Steam runtime for
# this Proton build. variant=fp8: the FP8 matrix variant, into fp8/.
variant=int8
fp8_config=
capture() {
    local status=0
    if [[ $variant == fp8 ]]; then
        rm -rf "$R/fp8/capture_$1_$2" "$R/fsr4cap.log"
        VKD3D_DISABLE_EXTENSIONS= DXIL_SPIRV_CONFIG=$fp8_config FSR4CAP_SUBDIR=fp8 \
            fsr4cap_run "$R" "$version" "$1" "$2" 3 || status=$?
    else
        rm -rf "$R/capture_$1_$2" "$R/fsr4cap.log"
        fsr4cap_run "$R" "$version" "$1" "$2" 3 || status=$?
    fi
    if [[ $status == 3 ]]; then
        return 3
    fi
    [[ $(tail -1 "$R/fsr4cap.log" 2>/dev/null || true) == *"frames done"* ]]
}
progress() {
    done_count=$((done_count + 1))
    printf '[%2d/%d] %-9s <- %-9s %s%s\n' "$done_count" "$total" "$2" "$1" \
        "$([[ $variant == fp8 ]] && echo 'FP8: ')" "$(tail -1 "$R/fsr4cap.log" 2>/dev/null || true)"
}

# The Proton build: the first under which the DLL starts FSR 4.1 (the first size's capture).
first=(1920x1080 1920x1080)
failures=()
no_runtime=0
chosen=
for candidate in "${candidates[@]}"; do
    export PROTONPATH=$candidate
    echo "Proton: $PROTONPATH"
    status=0
    capture "${first[@]}" || status=$?
    if [[ $status == 0 ]]; then
        chosen=$candidate
        break
    fi
    if [[ $status == 3 ]]; then
        no_runtime=$((no_runtime + 1))
        failures+=("${candidate##*/}: its Steam runtime is not installed")
    else
        # "no version matching": the DLL offered FSR 3/2 only under this vkd3d-proton.
        failures+=("${candidate##*/}: $(tail -1 "$R/fsr4cap.log" 2>/dev/null || tail -1 "$R/umu.log" 2>/dev/null || echo '?')")
    fi
    echo "  FSR 4.1 did not start under it; next" >&2
done
if [[ -z $chosen ]]; then
    echo "No Proton build started the DLL's FSR 4.1:" >&2
    printf '  %s\n' "${failures[@]}" >&2
    if [[ $no_runtime == "${#candidates[@]}" ]]; then
        exit 3
    fi
    echo "Either they are too old (GE-Proton 10+, Proton - Experimental or Proton-CachyOS 11" \
         "start it), or the DLL does not enable FSR 4.1 on this GPU (the Steam Deck's?): then build" \
         "the assets on a PC with a Radeon RX 7000/9000 and copy the fsr4_411 folder over." >&2
    tail -5 "$R/umu.log" >&2 2>/dev/null || true
    exit 5
fi
progress "${first[@]}"

# The FP8 variant: the same size with the matrices visible. Another variant when its shaders differ.
rm -rf "$R/fp8"
fp8=0
if [[ ${BB_FSR4CAP_FP8:-auto} != 0 ]]; then
    [[ ${BB_FSR4CAP_FP8:-auto} != 1 ]] || fp8_config=wmma_rdna3_workaround
    variant=fp8
    shaders() { (cd "$1" 2>/dev/null && ls cs_*.dxil 2>/dev/null); }
    if capture "${first[@]}" &&
       [[ $(shaders "$R/fp8/capture_${first[0]}_${first[1]}") != "$(shaders "$R/capture_${first[0]}_${first[1]}")" ]]; then
        fp8=1
        total=40
        progress "${first[@]}"
    else
        rm -rf "$R/fp8"
        echo "FP8 variant: not offered on this GPU (RDNA4 only), INT8 only"
    fi
    variant=int8
fi

record() { # record <render WxH> <output WxH>
    if [[ $1 == "${first[0]}" && $2 == "${first[1]}" ]]; then
        return # captured while choosing the Proton build
    fi
    capture "$1" "$2" || {
        if [[ $variant == fp8 ]]; then
            echo "The FP8 variant stopped running under $PROTONPATH: INT8 only." >&2
            tail -5 "$R/umu.log" >&2 2>/dev/null || true
            rm -rf "$R/fp8"
            return 1
        fi
        echo "The FSR 4.1.1 upscaler stopped running under $PROTONPATH. Log: $R/fsr4cap.log," \
             "$R/umu.log." >&2
        tail -5 "$R/umu.log" >&2 2>/dev/null || true
        exit 5
    }
    progress "$1" "$2"
}
record_all() {
    local out ow oh ratio c
    for out in 1920x1080 2560x1440 3840x2160; do
        ow=${out%x*}; oh=${out#*x}
        for ratio in 1.0 1.5 1.7 2.0 3.0; do
            record "$(awk -v w="$ow" -v h="$oh" -v r="$ratio" 'BEGIN { printf "%dx%d", int(w / r + 0.5), int(h / r + 0.5) }')" "$out" || return
        done
    done
    for c in '853x533 1280x800' '640x400 1280x800' '1067x600 1600x900' '1707x720 2560x1080' '2293x960 3440x1440'; do
        record $c || return
    done
}
record_all
if [[ $fp8 == 1 ]]; then
    variant=fp8
    record_all || true
    variant=int8
fi
