#!/bin/sh
# Hourly Nightwatch changelist sync on quad.fi: pull NightwatchCoop and import its git history into the
# clinic database (as nw-sync). One log line per run in ~/.config/netdoctor/nw_sync.log.
#   17 * * * * sh $HOME/bbport/tools/discord/nw_sync_cron.sh
SRC="${NW_SRC:-$HOME/nightwatch-src}"
DB="${NETDOCTOR_DB:-$HOME/.config/netdoctor/netdoctor.db}"
LOG="$HOME/.config/netdoctor/nw_sync.log"
PY="$HOME/netdoctor-venv/bin/python"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$(dirname "$LOG")"
say() { echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) $*" >> "$LOG"; }
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet https://github.com/vladtepes22/NightwatchCoop "$SRC" >>"$LOG" 2>&1 || { say "clone failed"; exit 1; }
fi
git -C "$SRC" pull --ff-only --quiet >>"$LOG" 2>&1 || { say "pull failed"; exit 1; }
out="$("$PY" "$HERE/cl_sync_nw.py" "$SRC" --local "$DB" 2>>"$LOG")" || { say "sync failed"; exit 1; }
say "$out"
