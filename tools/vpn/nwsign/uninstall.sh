#!/bin/sh
# Roll nwsign back. Restores the pre-nwsign server.conf (the old shared tls-crypt profile works again),
# closes the signing port, stops the service. Add --purge to also delete keys, state, user and code.
set -u
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
CONF=/etc/openvpn/server/server.conf
PORT=${NWSIGN_PORT:-8443}
if [ -f "$CONF.pre-nwsign" ]; then
  cp -a "$CONF.pre-nwsign" "$CONF" && systemctl restart openvpn-server@server
else
  echo "no $CONF.pre-nwsign found; server.conf left alone" >&2
fi
systemctl disable --now nwsign.service 2>/dev/null
iptables -D INPUT -i eth0 -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null
[ -f /etc/nftables.conf.pre-nwsign ] && cp -a /etc/nftables.conf.pre-nwsign /etc/nftables.conf
h=$(nft -a list chain inet filter input 2>/dev/null | sed -n "s/.*tcp dport $PORT accept # handle \([0-9]*\).*//p" | head -1)
[ -n "$h" ] && nft delete rule inet filter input handle "$h"
for f in /etc/iptables/add-openvpn-rules.sh /etc/iptables/rm-openvpn-rules.sh; do
  [ -f "$f.pre-nwsign" ] && cp -a "$f.pre-nwsign" "$f"
done
rm -f /etc/systemd/system/nwsign.service /usr/local/sbin/nwsign-admin /etc/openvpn/server/ca-bundle.crt
systemctl daemon-reload
if [ "${1:-}" = "--purge" ]; then
  rm -rf /opt/nwsign /var/lib/nwsign /etc/nwsign
  userdel nwsign 2>/dev/null
fi
echo "nwsign removed. Every per-install profile stops working; the old shared profile works again."
