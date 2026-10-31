#!/bin/sh
# Undo cutover.sh from its latest backup (/root/nw-cutover-backup/latest): server.conf, both firewall scripts
# (re-applied), the status-file drop-in, nwlobby's code and unit. Restarts OpenVPN once (everyone reconnects).
#   sudo sh rollback.sh [backup dir]
set -eu
B=${1:-/root/nw-cutover-backup/latest}
CONF=/etc/openvpn/server/server.conf
FW=/etc/iptables/add-openvpn-rules.sh
FWRM=/etc/iptables/rm-openvpn-rules.sh
UNIT=openvpn-server@server.service
DROPIN=/etc/systemd/system/$UNIT.d/nightwatch-no-status.conf
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
[ -f "$B/server.conf" ] || { echo "no backup in $B" >&2; exit 1; }
echo "rolling back from $(readlink -f "$B")"
"$FWRM" >/dev/null 2>&1 || true                  # the current (/22) rules out
cp -a "$B/server.conf" "$CONF"
cp -a "$B/$(basename "$FW")" "$FW"
cp -a "$B/$(basename "$FWRM")" "$FWRM"
"$FW"                                            # the backed-up rules in
[ -d "$B/ccd" ] && cp -a "$B/ccd/." /etc/openvpn/server/ccd/
if [ -f "$B/$(basename "$DROPIN")" ]; then cp -a "$B/$(basename "$DROPIN")" "$DROPIN"; else rm -f "$DROPIN"; fi
if [ -f "$B/nwlobby.py" ]; then
  cp -a "$B/nwlobby.py" /opt/nwlobby/nwlobby.py
  [ -f "$B/nwlobby.service" ] && cp -a "$B/nwlobby.service" /etc/systemd/system/nwlobby.service
fi
systemctl daemon-reload
systemctl restart "$UNIT"
sleep 3
systemctl restart nwlobby 2>/dev/null || true
echo "OpenVPN: $(systemctl is-active $UNIT), tun0 $(ip -4 -br addr show tun0 | awk '{print $3}'), nwlobby: $(systemctl is-active nwlobby)"
