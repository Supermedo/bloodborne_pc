#!/usr/bin/env bash
# tools/visual.sh NAME [ENV=VALUE...]: enters the level (tools/restart.sh), then captures the game
# window standing still, after turning the camera and after a few steps forward
# (out/visual_NAME_{1,2,3}.png; VISUAL_WALK=0: no steps), then closes the game. The log is
# out/visual_NAME.log.
set -u
cd -- "$(dirname -- "$0")/.."
name=${1:?usage: tools/visual.sh NAME [ENV=VALUE...]}
shift
env BB_GAME_DIR="${BB_GAME_DIR:-$PWD/../game_files/CUSA03173}" "$@" tools/restart.sh > /dev/null || exit 1
sleep 6; tools/shot.sh "out/visual_${name}_1.png"
tools/press.sh rx=255 1.2; sleep 2; tools/shot.sh "out/visual_${name}_2.png"
if [[ ${VISUAL_WALK:-1} == 1 ]]; then
    tools/press.sh ly=0 2.5; sleep 1.5; tools/shot.sh "out/visual_${name}_3.png"
fi
tools/press.sh rx=0 1.2; sleep 2; tools/shot.sh "out/visual_${name}_4.png"
sleep "${HOLD:-5}"
tools/stop_own.sh
cp out/session.log "out/visual_${name}.log"
