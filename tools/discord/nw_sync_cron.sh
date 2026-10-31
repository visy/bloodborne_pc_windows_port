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
# A read-only mirror: follow the remote branch even after a force-push (an amended commit made --ff-only fail,
# 2026-10-31). Nothing local lives here.
BR="$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null)"
git -C "$SRC" fetch --quiet origin >>"$LOG" 2>&1 || { say "fetch failed"; exit 1; }
if ! git -C "$SRC" merge-base --is-ancestor HEAD "origin/$BR" 2>/dev/null; then
  say "remote history rewritten (force-push): resetting the mirror to origin/$BR"
fi
git -C "$SRC" reset --quiet --hard "origin/$BR" >>"$LOG" 2>&1 || { say "reset failed"; exit 1; }
out="$("$PY" "$HERE/cl_sync_nw.py" "$SRC" --local "$DB" 2>>"$LOG")" || { say "sync failed"; exit 1; }
say "$out"
