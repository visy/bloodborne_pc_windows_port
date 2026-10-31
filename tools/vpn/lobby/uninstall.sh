#!/bin/sh
# Roll nwlobby back: stop the service, remove the code and the firewall rules install.sh added.
# (The Nightwatch iptables script's own NW_VPN_IN rule for 9310 is not ours and stays.)
set -u
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
PORT=9310
systemctl disable --now nwlobby.service 2>/dev/null
rm -f /etc/systemd/system/nwlobby.service
systemctl daemon-reload
rm -rf /opt/nwlobby
# iptables: the live rule we inserted and our lines in the persisted scripts
iptables -D INPUT -i tun+ -s 10.8.0.0/22 -p udp --dport "$PORT" -j ACCEPT 2>/dev/null
for f in /etc/iptables/add-openvpn-rules.sh /etc/iptables/rm-openvpn-rules.sh; do
  [ -f "$f" ] && sed -i '/# nwlobby$/d' "$f"
done
# nftables: the live rule (by handle) and our line in the file
for h in $(nft -a list chain inet filter input 2>/dev/null | sed -n "s/.*iifname \"tun0\" udp dport $PORT accept.*# handle \([0-9]*\).*/\1/p"); do
  nft delete rule inet filter input handle "$h"
done
for h in $(nft -a list chain inet filter input 2>/dev/null | sed -n 's/.*iifname "lo" udp dport 9311 accept.*# handle \([0-9]*\).*/\1/p'); do
  nft delete rule inet filter input handle "$h"
done
[ -f /etc/nftables.conf ] && sed -i '/# nwlobby$/d' /etc/nftables.conf && nft -c -f /etc/nftables.conf
echo "nwlobby removed."
