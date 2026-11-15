#!/usr/bin/env bash
# Start a bbport / Nightwatch engine folder (run.bat) under Proton, scripted, and quit after N seconds.
#   proton_bbport.sh <engine-dir> <game-dir> <log-dir> [seconds=300] [save-dir]
# Test-harness environment (silent, windowed, no launcher settings). The pad file is written by this
# script: Continue (cross) is tapped every 3 s once the log shows the title, until the world is up.
# Console output of run.bat goes to <log-dir>/game.log. Run it while holding a game slot
# (tools/linux/with_game_slot.py). EXTRA_ENV="A=1 B=2" adds game / Proton variables; PREFIX picks the wine prefix;
# WINEDEBUG / PROTON_WINEDEBUG go to proton_run.sh.
set -uo pipefail
ENG=$1; GAME=$2; LOG=$3; SECS=${4:-300}; SAVES=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$LOG"
BS=$(printf '\134'); CR=$(printf '\r')
winpath() { echo "Z:${1////$BS}"; }
if [[ -n $SAVES ]]; then rm -rf "$ENG/user/savedata"; mkdir -p "$ENG/user"; cp -r "$SAVES" "$ENG/user/savedata"; fi
PAD=$LOG/pad.txt; : > "$PAD"
# Engine folder (bbport release: run.bat) or Nightwatch package (Bloodborne.exe -> launcher --play -> engine start;
# its settings come from <prefix>/.../AppData/Roaming/nightwatch/settings.json, see proton_nw_settings.py; the game's
# console output is then logs/last_session.log).
if [[ -f $ENG/run.bat ]]; then
  START="call run.bat"; GAMELOG=$LOG/game.log
else
  START="call Bloodborne.exe"; GAMELOG=$ENG/logs/last_session.log; rm -f "$GAMELOG"
fi
# go.bat: the start with its console output kept (a start /unix launch loses it) in <log>/game.log
{
  echo "@echo off$CR"
  echo "cd /d \"$(winpath "$ENG")\"$CR"
  echo "$START > \"$(winpath "$LOG")${BS}game.log\" 2>&1$CR"
  echo "echo go.bat: run.bat ended, errorlevel %ERRORLEVEL% >> \"$(winpath "$LOG")${BS}game.log\"$CR"
} > "$LOG/go.bat"
export BB_GAME_DIR=$(winpath "$GAME") BB_FROM_LAUNCHER=1 BB_NO_LAUNCHER_SETTINGS=1 BB_FULLSCREEN=0 BB_AUDIO=none
export BB_VRAM_LIMIT_MB=${BB_VRAM_LIMIT_MB:-4096} BB_SHADER_PRECOMPILE=1 BB_DLSS=0 BB_PAD_FILE=$(winpath "$PAD")
export BB_PATCHES="Skip Online/Offline Choice;Skip Intro" BB_SHOT_TRIGGER=$(winpath "$LOG/shot.trigger") BB_SHOT_DIR=$(winpath "$LOG/shotdump")
export BB_SHADER_THREADS=2 BB_ASYNC_PIPELINE_THREADS=2 BB_PREP_WORKERS=1 BB_FPS=${BB_FPS:-30} BB_WINDOW_BACKGROUND=0
export BB_TRACE=1 BB_BOOT_TIMES=1
( "$HERE/proton_run.sh" ${SHOT_EVERY:+-S $SHOT_EVERY} -t "$SECS" -l "$LOG" -p "${PREFIX:-$HOME/proton082/prefix/default}" "$LOG/go.bat" ) &
RUNPID=$!
t0=$(date +%s); pressed=0
while kill -0 $RUNPID 2>/dev/null; do
  sleep 3
  if grep -qs "Boot: .*title" "$GAMELOG" && [[ $pressed -lt 200 ]]; then
    echo cross > "$PAD"; sleep 0.25; : > "$PAD"; pressed=$((pressed+1))
  fi
  if grep -qs "Boot: .*in the world" "$GAMELOG"; then
    echo "[proton_bbport] world reached after $(( $(date +%s)-t0 )) s"; sleep 20
    pkill -TERM -P $RUNPID 2>/dev/null; break
  fi
done
wait $RUNPID 2>/dev/null
[[ $GAMELOG != "$LOG/game.log" ]] && cp "$GAMELOG" "$LOG/last_session.log" 2>/dev/null
echo "[proton_bbport] done, $(( $(date +%s)-t0 )) s, presses $pressed"
