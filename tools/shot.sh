#!/usr/bin/env bash
# tools/shot.sh FILE.png: captures only the game window (bb-probe) with grim, by its Hyprland
# geometry (hyprctl clients: at, size, pid per window block).
pid=$(pgrep -x bb-probe | head -1)
[[ -n $pid ]] || { echo "no game"; exit 1; }
geo=$(hyprctl clients | awk -v pid="$pid" '
    /^Window / { at=""; size="" }
    /^\tat: / { at=$2 }
    /^\tsize: / { size=$2 }
    /^\tpid: / && $2 == pid { split(size, s, ","); print at " " s[1] "x" s[2]; exit }')
[[ -n $geo ]] || { echo "no window for $pid"; exit 1; }
grim -g "$geo" "$1" && echo "$1 ($geo)"
