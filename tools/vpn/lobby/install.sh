#!/bin/sh
# nwlobby installer for the Nightwatch VPN server (Debian 13). Run as root from a copy of this directory:
#     sudo sh install.sh
# Idempotent: safe to run again (refreshes code and unit, adds each firewall rule only once).
# Roll back with uninstall.sh. Needs nothing from the OpenVPN setup except the tunnel (tun0, 10.8.0.0/24).
set -eu

APP=/opt/nwlobby
PORT=${NWLOBBY_PORT:-9310}
FW=/etc/iptables/add-openvpn-rules.sh
FWRM=/etc/iptables/rm-openvpn-rules.sh
NFT=/etc/nftables.conf
SRC=$(cd "$(dirname "$0")" && pwd)

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
for f in nwlobby.py nwlobby.service; do [ -f "$SRC/$f" ] || die "missing $SRC/$f"; done
command -v python3 >/dev/null || die "need python3"
[ "$PORT" = 9310 ] || die "the unit, the launcher and the firewall text use 9310; edit them together to change it"
python3 -I -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())' "$SRC/nwlobby.py" || die "nwlobby.py does not parse"

# ---- code ----------------------------------------------------------------------------------------
install -d -m 755 -o root -g root "$APP"
install -m 644 -o root -g root "$SRC/nwlobby.py" "$APP/nwlobby.py"

# ---- firewall 1: the iptables-nft rules (persisted through the existing rules scripts) ------------
# The Nightwatch rules already accept UDP 9310 from the tunnel (chain NW_VPN_IN); only add a rule when
# the script has none. The live rule is checked the same way, so a second run changes nothing.
if [ -f "$FW" ] && ! grep -q -F -e "LOBBY=$PORT" -e "--dport $PORT " "$FW"; then
  cp -an "$FW" "$FW.pre-nwlobby"; [ -f "$FWRM" ] && cp -an "$FWRM" "$FWRM.pre-nwlobby"
  sed -i "/--dport 1194 -j ACCEPT/a iptables -I INPUT 1 -i tun+ -s 10.8.0.0/24 -p udp --dport $PORT -j ACCEPT   # nwlobby" "$FW"
  [ -f "$FWRM" ] && sed -i "/--dport 1194 -j ACCEPT/a iptables -D INPUT -i tun+ -s 10.8.0.0/24 -p udp --dport $PORT -j ACCEPT 2>/dev/null   # nwlobby" "$FWRM"
fi
if command -v iptables >/dev/null; then
  if ! iptables -S 2>/dev/null | grep -q -- "-p udp .*--dport $PORT .*-j ACCEPT"; then
    iptables -I INPUT 1 -i tun+ -s 10.8.0.0/24 -p udp --dport "$PORT" -j ACCEPT
  fi
fi

# ---- firewall 2: the nftables "inet filter" table (input policy drop), live and in the file -------
if command -v nft >/dev/null && nft list chain inet filter input >/dev/null 2>&1; then
  nft list chain inet filter input | grep -q "udp dport $PORT accept" || \
    nft add rule inet filter input iifname "tun0" udp dport "$PORT" accept
  if [ -f "$NFT" ] && ! grep -q "udp dport $PORT accept" "$NFT"; then
    cp -an "$NFT" "$NFT.pre-nwlobby"
    sed -i "/^[[:space:]]*udp dport 1194 accept/a\        iifname \"tun0\" udp dport $PORT accept        # nwlobby" "$NFT"
    nft -c -f "$NFT" || { cp -a "$NFT.pre-nwlobby" "$NFT"; die "nftables.conf edit did not check; restored"; }
  fi
fi

# ---- service ---------------------------------------------------------------------------------------
install -m 644 "$SRC/nwlobby.service" /etc/systemd/system/nwlobby.service
systemctl daemon-reload
systemctl enable nwlobby.service >/dev/null 2>&1
systemctl restart nwlobby.service
# The service waits for 10.8.0.1 itself (openvpn must be up); check that it bound the port.
OK=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
  if systemctl is-active --quiet nwlobby.service; then
    command -v ss >/dev/null || { OK=1; break; }
    ss -H -lnu 2>/dev/null | grep -q "10.8.0.1:$PORT" && { OK=1; break; }
  fi
done
[ $OK = 1 ] || { journalctl -u nwlobby -n 20 --no-pager >&2; die "nwlobby is not listening on 10.8.0.1:$PORT (is openvpn-server@server up?)"; }

cat <<MSG

nwlobby installed: UDP 10.8.0.1:$PORT, reachable from the tunnel only.
  status:    systemctl status nwlobby
  roll back: sudo sh uninstall.sh
MSG
