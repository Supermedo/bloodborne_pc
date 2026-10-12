#!/usr/bin/env bash
# Load watch while the game runs: bash tools/load_watch.sh [GAME_LOG]
# Every 5 s one line in out/load_<time>.log: GPU busy (average/minimum of 0.25 s samples), shader
# clock, the game's CPU use (100% = one core) and its busiest threads, and the game log's latest
# FPS. A thread near 100% while the GPU is under ~95% is what holds the frame rate.
# Memory: the game's VRAM and GTT (guest memory shared with the GPU) from its DRM fdinfo, and RSS.
cd "$(dirname "$0")/.."
log=${1:-$(ls -t out/ab_*.log 2>/dev/null | head -1)}
busy=$(ls /sys/class/drm/card*/device/gpu_busy_percent | head -1)
dev=$(dirname "$busy")
out=out/load_$(date +%m%d_%H%M%S).log
echo "Load log: $out (game log $log)"
hz=$(getconf CLK_TCK)
snapshot() { # tid name ticks
    for s in /proc/$1/task/*/stat; do
        read -r line < "$s" 2>/dev/null || continue
        name=${line#*(}; name=${name%)*}; rest=${line##*) }
        set -- $rest
        echo "${s#/proc/*/task/}" "${name// /_}" $(( ${12} + ${13} ))
    done
}
while :; do
    pid=$(pgrep -x bb-probe | head -1)
    if [[ -z $pid ]]; then
        echo "$(date +%H:%M:%S) game not running" >> "$out"
        sleep 5
        continue
    fi
    before=$(snapshot "$pid")
    t0=$(date +%s%N)
    sum=0; n=0; min=100
    for _ in $(seq 20); do
        v=$(cat "$busy"); sum=$((sum + v)); n=$((n + 1)); ((v < min)) && min=$v
        sleep 0.25
    done
    after=$(snapshot "$pid")
    t1=$(date +%s%N)
    threads=$(awk -v hz="$hz" -v secs="$(( (t1 - t0) / 1000000 ))" '
        NR == FNR { split($0, a, " "); before[a[1]] = a[3]; next }
        { split($0, a, " "); id = a[1]; sub(/\/stat$/, "", id); name = a[2]
          if (!(a[1] in before)) next
          d = a[3] - before[a[1]]; if (d <= 0) next
          pct = d * 100000 / hz / secs; total += pct
          # Same-named workers summed (HavokWorkerThre x N), the busiest one kept apart.
          group[name] += pct; count[name]++; if (pct > top[name]) top[name] = pct }
        END {
          printf "cpu %.0f%% |", total
          n = 0
          for (name in group) order[++n] = name
          for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
              if (group[order[j]] > group[order[i]]) { t = order[i]; order[i] = order[j]; order[j] = t }
          for (i = 1; i <= n && i <= 7; i++) {
              name = order[i]
              if (count[name] > 1) printf " %s x%d %.0f%% (max %.0f%%)", name, count[name], group[name], top[name]
              else printf " %s %.0f%%", name, group[name]
          }
        }' <(echo "$before" | sed 's|/stat||') <(echo "$after" | sed 's|/stat||'))
    mem=$(awk '/^drm-memory-vram:/ {v += $2} /^drm-memory-gtt:/ {g += $2} END {printf "vram %.0f MiB gtt %.0f MiB", v / 1024, g / 1024}' $(grep -l "^drm-driver" /proc/$pid/fdinfo/* 2>/dev/null) 2>/dev/null)
    rss=$(awk '/^VmRSS:/ {printf "rss %.0f MiB", $2 / 1024}' /proc/$pid/status 2>/dev/null)
    fps=$(grep "^Frame stats" "$log" 2>/dev/null | tail -1 | sed -E 's/Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*/\1 FPS worst \2 ms/')
    echo "$(date +%H:%M:%S) gpu $((sum / n))% min ${min}% sclk $(grep '\*' "$dev/pp_dpm_sclk" | awk '{print $2}') | $fps | $mem $rss | $threads" >> "$out"
done
