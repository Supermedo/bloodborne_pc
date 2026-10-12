#!/usr/bin/env bash
# tools/soak_walk.sh SECS [ENV=VALUE...]: enters the level, then walks and turns in a loop (area
# streaming), reports survival; the log ends up in out/soak_walk_<time>.log.
cd -- "$(dirname -- "$0")/.."
secs=${1:?usage: tools/soak_walk.sh SECS [ENV=VALUE...]}
shift
env BB_GAME_DIR="${BB_GAME_DIR:-$PWD/../game_files/CUSA03173}" "$@" tools/restart.sh > /dev/null || exit 1
end=$((SECONDS + secs))
status=alive
while ((SECONDS < end)); do
    tools/press.sh "ly=0" 3; tools/press.sh "rx=40" 1.5; tools/press.sh "ly=0" 3
    tools/press.sh "ly=255" 3; tools/press.sh "rx=215" 1.5; sleep 1
    if ! pgrep -x bb-probe > /dev/null; then status="DIED after $SECONDS s"; break; fi
done
tools/stop_own.sh
log=out/soak_walk_$(date +%m%d_%H%M%S).log
cp out/session.log "$log"
echo "$status: $log; $(grep -cE 'Fault \(signal|Device lost|STOP|Assertion' "$log") errors; $(grep '^Frame stats' "$log" | tail -1 | cut -c1-40)"
