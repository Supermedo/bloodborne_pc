#!/usr/bin/env bash
# tools/restart.sh [run.sh args]: restarts the game with frame stats, BB_PAD_FILE and
# BB_TOGGLE_FILE (out/pad, out/toggles) and enters the level (title: cross, cross).
# Only a game these tools started is restarted (out/tools_game.pid): with another one running (the
# user's) nothing happens and it exits with 1; FORCE_RESTART=1 closes that one too.
cd -- "$(dirname -- "$0")/.."
T=tools
own=$(cat out/tools_game.pid 2>/dev/null)
for pid in $(pgrep -x bb-probe); do
    if [[ $pid != "$own" ]]; then
        if [[ ${FORCE_RESTART:-0} != 1 ]]; then
            echo "restart.sh: a game these tools did not start is running (pid $pid): left alone" >&2
            exit 1
        fi
        kill "$pid"; sleep 2; kill -9 "$pid" 2>/dev/null
    fi
done
$T/stop_own.sh; sleep 1
: > out/pad; echo ${BASE_MASK:-0} > out/toggles
BB_PAD_FILE=$PWD/out/pad BB_FRAME_STATS=1 BB_TOGGLE_FILE=$PWD/out/toggles BB_FPS_LIMIT=${BB_FPS_LIMIT:-0} \
    ${CPUS:+taskset -c $CPUS} setsid stdbuf -oL -eL bash run.sh "$@" > out/session.log 2>&1 < /dev/null &
# The game's process (run.sh starts it after preparing the files): the only bb-probe now.
for i in $(seq 1 120); do
    sleep 0.5
    pid=$(pgrep -n -x bb-probe) && { echo "$pid" > out/tools_game.pid; break; }
done
# Title menu: the pad is opened and frame stats report a light scene for a while.
for i in $(seq 1 180); do
    sleep 1
    [[ $(grep -c '^Frame stats' out/session.log) -ge 3 ]] && grep -q 'pad opened' out/session.log && break
done
sleep ${TITLE_WAIT:-6}
$T/press.sh cross; sleep 6; $T/press.sh cross
for i in $(seq 1 90); do
    sleep 2
    last=$(grep '^Frame stats' out/session.log | tail -1 | sed -E 's/.* ([0-9]+) draws\/frame.*/\1/')
    [[ -n $last && $last -gt 600 ]] && break
done
sleep 10
grep '^Frame stats' out/session.log | tail -1
