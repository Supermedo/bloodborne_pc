#!/usr/bin/env bash
# tools/stop_own.sh: closes the game tools/restart.sh started (out/tools_game.pid), never another
# one: a game the user started (or is playing) is left alone.
cd -- "$(dirname -- "$0")/.."
pid=$(cat out/tools_game.pid 2>/dev/null)
rm -f out/tools_game.pid
[[ -n $pid && $(cat /proc/$pid/comm 2>/dev/null) == bb-probe ]] || exit 0
kill "$pid"
for i in 1 2 3; do
    sleep 1
    kill -0 "$pid" 2>/dev/null || exit 0
done
kill -9 "$pid" 2>/dev/null
exit 0
