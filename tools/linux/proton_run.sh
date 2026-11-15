#!/usr/bin/env bash
# Run a Windows program under Proton (umu-launcher + Proton-GE) with logs, for testing on Linux / WSL2.
#   proton_run.sh [options] <exe-or-bat> [args...]
#   -l DIR   log folder (default $ROOT/runs/<name>-<time>)    -p DIR  wine prefix (default $ROOT/prefix/default)
#   -t SEC   kill the run after SEC seconds (default 600; a run never idles forever)
#   -S SEC   screenshot every window of the X display every SEC seconds into <log>/shots (ImageMagick import + xwininfo)
#   -s       software Vulkan (lavapipe, default in WSL)   -g  use the system's default Vulkan ICDs instead
# Environment: ROOT (default ~/proton082), PROTON_GE (default newest under $ROOT/proton), WINEDEBUG (default
#   err+all,fixme-all in the log, +loaddll,+module with PROTON_WINEDEBUG=1), EXTRA_ENV="A=1 B=2" to pass more.
# Logs: <log>/steam-umu-0.log (PROTON_LOG), <log>/dxvk/*.log, <log>/vkd3d.log, <log>/run.log (stdout+stderr), <log>/env.txt
set -uo pipefail
ROOT=${ROOT:-$HOME/proton082}
LOG=""; PREFIX=$ROOT/prefix/default; SECS=600; SOFT=1; SHOTS=0
while getopts "l:p:t:sgS:" o; do case $o in l) LOG=$OPTARG;; p) PREFIX=$OPTARG;; t) SECS=$OPTARG;; s) SOFT=1;; g) SOFT=0;; S) SHOTS=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
[[ $# -ge 1 ]] || { sed -n 2,12p "$0"; exit 2; }
EXE=$1; shift
name=$(basename "$EXE" | tr ' ' '_')
[[ -n $LOG ]] || LOG=$ROOT/runs/$name-$(date +%H%M%S)
mkdir -p "$LOG/dxvk" "$PREFIX"
PROTON_GE=${PROTON_GE:-$(ls -d "$ROOT"/proton/GE-Proton*/ | sort -V | tail -1)}
# a clean PATH: WSL's Windows interop PATH confuses umu / Proton (spaces, .exe lookups)
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/wsl/lib
export WINEPREFIX=$PREFIX GAMEID=${GAMEID:-umu-0} PROTONPATH=${PROTON_GE%/}
export PROTON_LOG=1 PROTON_LOG_DIR=$LOG
export WINEDEBUG=${WINEDEBUG:--all,+err}
[[ ${PROTON_WINEDEBUG:-0} == 1 ]] && export WINEDEBUG=err+all,+loaddll,+module,+seh
export DXVK_LOG_PATH=$LOG/dxvk DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info}
export VKD3D_DEBUG=${VKD3D_DEBUG:-warn} VKD3D_LOG_FILE=$LOG/vkd3d.log
export DISPLAY=${DISPLAY:-:0}
if [[ $SOFT == 1 ]]; then
  export VK_DRIVER_FILES=${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}
  export LP_NUM_THREADS=${LP_NUM_THREADS:-8}
fi
for kv in ${EXTRA_ENV:-}; do export "$kv"; done
env | grep -E '^(WINE|PROTON|DXVK|VKD3D|VK_|BB_|DISPLAY|WAYLAND|GAMEID|LP_)' | sort > "$LOG/env.txt"
[[ -f $EXE ]] && cd "$(dirname "$EXE")"
if [[ $SHOTS -gt 0 ]]; then
  mkdir -p "$LOG/shots"
  ( n=0; while sleep "$SHOTS"; do n=$((n+1)); xwininfo -root -tree > "$LOG/shots/tree-$n.txt" 2>&1
      i=0; for w in $(grep -oE '^ +0x[0-9a-f]+ "[^"]+"' "$LOG/shots/tree-$n.txt" | awk '{print $1}'); do
        i=$((i+1)); import -window "$w" "$LOG/shots/w$n-$i.png" 2>/dev/null; done; done ) &
  SHOTPID=$!
fi
echo "[proton_run] $(date +%T) start $EXE $* (log $LOG, limit ${SECS}s)" | tee "$LOG/run.log"
timeout -k 20 "$SECS" python3 "$ROOT/umu/umu/umu-run" "$EXE" "$@" >> "$LOG/run.log" 2>&1
rc=$?
[[ -n ${SHOTPID:-} ]] && kill $SHOTPID 2>/dev/null
echo "[proton_run] $(date +%T) exit $rc" | tee -a "$LOG/run.log"
exit $rc
