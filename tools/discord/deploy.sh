#!/bin/sh
# Deploy Iosefka on quad.fi: pull, test, restart, check. Any failure rolls back to the previous commit.
#   sh tools/discord/deploy.sh [--dry-run]
# Steps: 1 fetch + record the current commit   2 git pull --ff-only   3 clinic tests with the venv python
# (pytest is installed into the venv with `uv pip install --python <venv python> pytest` when missing, else
# `python -m pip install pytest`)   4 kill her process (exact pattern) and start it with keepalive.sh
# 5 wait for the "netdoctor bot ready" line in bot.log   6 `iosefka_cli.py --as visy health`.
# On failure: git reset --hard to the recorded commit, restart, and a health alert (Discord REST).
# --dry-run only prints the steps (it still reads the current commit when the repo is there).
# Overrides: REPO, VENV, NETDOCTOR_DIR (config dir), READY_WAIT (seconds, default 90).
DRY=0
[ "$1" = "--dry-run" ] && DRY=1
REPO="${REPO:-$HOME/bbport}"
VENV="${VENV:-$HOME/netdoctor-venv}"
CONF="${NETDOCTOR_DIR:-$HOME/.config/netdoctor}"
PY="$VENV/bin/python"
LOG="$CONF/bot.log"
D="$REPO/tools/discord"
READY_WAIT="${READY_WAIT:-90}"
PATTERN='[t]ools/discord/netdoctor_bot.py'

say() { echo "deploy: $*"; }
run() {
    if [ "$DRY" = 1 ]; then echo "  would run: $*"; return 0; fi
    "$@"
}

PREV=""
if [ "$DRY" = 1 ]; then
    say "DRY RUN, nothing is changed"
    PREV="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo '<current commit>')"
else
    PREV="$(git -C "$REPO" rev-parse HEAD)" || { say "no git repo at $REPO"; exit 1; }
fi

restart() {
    say "restart: kill by pattern $PATTERN, then keepalive.sh"
    run pkill -u "$(id -u)" -f "$PATTERN"
    if [ "$DRY" != 1 ]; then
        n=0
        while pgrep -u "$(id -u)" -f "$PATTERN" >/dev/null && [ $n -lt 20 ]; do sleep 1; n=$((n + 1)); done
    fi
    run sh "$D/keepalive.sh"
}

wait_ready() {
    say "wait up to ${READY_WAIT}s for the ready line in $LOG"
    if [ "$DRY" = 1 ]; then echo "  would wait for: netdoctor bot ready as"; return 0; fi
    n=0
    while [ $n -lt "$READY_WAIT" ]; do
        tail -c +"$((START_SIZE + 1))" "$LOG" 2>/dev/null | grep -q 'netdoctor bot ready as' && return 0
        sleep 1; n=$((n + 1))
    done
    return 1
}

fail() {
    say "FAILED at step: $1"
    if [ "$DRY" = 1 ]; then
        echo "  would run: git -C $REPO reset --hard $PREV"; echo "  would restart and send an alert"; exit 1
    fi
    git -C "$REPO" reset --hard "$PREV"
    restart
    "$PY" "$D/clinic_health.py" alert "deploy failed at '$1'; rolled back to ${PREV%${PREV#???????}}." || true
    exit 1
}

say "1/6 fetch and record the current commit ($PREV)"
run git -C "$REPO" fetch --quiet || fail fetch
say "2/6 pull --ff-only"
run git -C "$REPO" pull --ff-only --quiet || fail pull
say "3/6 clinic tests with $PY"
if [ "$DRY" = 1 ]; then
    echo "  would run: $PY -c 'import pytest' || uv pip install --python $PY pytest (or pip)"
else
    "$PY" -c 'import pytest' 2>/dev/null || { uv pip install --python "$PY" pytest || "$PY" -m pip install pytest; } || fail "install pytest"
fi
if [ "$DRY" = 1 ]; then echo "  would run: (cd $REPO && $PY -m pytest tests -q -k clinic)"
else (cd "$REPO" && "$PY" -m pytest tests -q -k clinic) || fail tests; fi
say "4/6 restart"
START_SIZE=0
[ "$DRY" = 1 ] || START_SIZE="$(wc -c < "$LOG" 2>/dev/null || echo 0)"
restart
say "5/6 ready line"
wait_ready || fail "ready line"
say "6/6 health"
if [ "$DRY" = 1 ]; then echo "  would run: $PY $D/iosefka_cli.py --as visy health --text"
else "$PY" "$D/iosefka_cli.py" --as visy health --text || fail health; fi
say "done"
