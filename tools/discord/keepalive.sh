#!/bin/sh
# Start the netdoctor bot if it is not running (for servers without systemd user lingering).
# crontab -e:
#   @reboot      sh $HOME/bbport/tools/discord/keepalive.sh
#   */5 * * * *  sh $HOME/bbport/tools/discord/keepalive.sh
pgrep -u "$(id -u)" -f 'tools/discord/netdoctor_bot.py' >/dev/null && exit 0
set -a
. "$HOME/.config/netdoctor/env"
set +a
cd "$HOME" || exit 1
nohup "$HOME/netdoctor-venv/bin/python" -u "$HOME/bbport/tools/discord/netdoctor_bot.py" \
    >> "$HOME/.config/netdoctor/bot.log" 2>&1 &
