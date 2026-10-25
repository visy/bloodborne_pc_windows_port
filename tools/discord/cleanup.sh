#!/bin/sh
# netdoctor bot housekeeping. The bot deletes each run's temporary folder itself; this catches what a
# crash or a kill mid-run left behind, and keeps the bot's own log small.
# crontab -e:
#   17 * * * *  sh $HOME/bbport/tools/discord/cleanup.sh
TMPROOT="${TMPDIR:-/tmp}"
# leftover upload folders older than an hour (only our own)
find "$TMPROOT" -maxdepth 1 -type d -name 'netdoctor_*' -user "$(id -u)" -mmin +60 -exec rm -rf {} + 2>/dev/null
# the bot log: keep the last ~2 MB once it passes 10 MB; the previous log file goes after a week
LOG="$HOME/.config/netdoctor/bot.log"
if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 10485760 ]; then
    tail -c 2097152 "$LOG" > "$LOG.tmp" && cat "$LOG.tmp" > "$LOG" && rm -f "$LOG.tmp"
fi
find "$HOME/.config/netdoctor" -maxdepth 1 -name 'bot.log.old' -mtime +7 -delete 2>/dev/null
exit 0
