#!/bin/sh
# Nightwatch VPN cutover for the 0.7.8 / NW update: widen the client pool from 10.8.0.0/24 (253) to /22 (1021),
# raise max-clients, drop the OpenVPN status file, refresh nwlobby. ONE OpenVPN restart: every player drops and
# reconnects by itself within ~30 s. Run it only once the NW build that accepts 10.8.0.0/22 is out (players on an
# older build who get a 10.8.1.x-10.8.3.x address can connect but cannot use the game list).
#
#   sudo sh cutover.sh --check      what would change (nothing is touched)
#   sudo sh cutover.sh              do it (asks to confirm, shows the connected count first)
#   sudo sh cutover.sh --yes        do it without asking
#   sudo sh rollback.sh             undo from the backup this script made
#
# Steps: backup -> old firewall rules out -> /22 in both firewall scripts (+ re-apply) -> server.conf pool and
# max-clients -> systemd drop-in without --status -> nwlobby (from ../lobby) -> OpenVPN restart -> checks.
# Any failed step before the restart restores the backup and stops; a failed check after it says to run rollback.sh.
set -eu
MAX_CLIENTS=${MAX_CLIENTS:-500}      # one OpenVPN core carries ~500 (measured 2026-10-31); DCO or a 2nd instance for more
NEW_MASK=255.255.252.0               # /22
NEW_NET=10.8.0.0/22
OLD_NET=10.8.0.0/24
CONF=/etc/openvpn/server/server.conf
CCD=/etc/openvpn/server/ccd
FW=/etc/iptables/add-openvpn-rules.sh
FWRM=/etc/iptables/rm-openvpn-rules.sh
UNIT=openvpn-server@server.service
DROPIN_DIR=/etc/systemd/system/$UNIT.d
DROPIN=$DROPIN_DIR/nightwatch-no-status.conf
SRC=$(cd "$(dirname "$0")" && pwd)
LOBBY_SRC=$(cd "$SRC/../lobby" 2>/dev/null && pwd || true)
BK=/root/nw-cutover-backup
MODE=run
for a in "$@"; do
  case "$a" in
    --check) MODE=check ;;
    --yes) MODE=yes ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

die() { echo "cutover: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
for f in "$CONF" "$FW" "$FWRM"; do [ -f "$f" ] || die "missing $f"; done
[ -n "$LOBBY_SRC" ] && [ -f "$LOBBY_SRC/install.sh" ] && [ -f "$LOBBY_SRC/nwlobby.py" ] || die "missing ../lobby (copy tools/vpn next to this script)"
grep -q 'DEFAULT_NET = "10.8.0.0/22"' "$LOBBY_SRC/nwlobby.py" || die "../lobby/nwlobby.py is not the /22 version"

clients() {
  HOME_OP=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
  NWVPN_PW="$HOME_OP/nwvpn/mgmt.pw" python3 -I "$HOME_OP/nwvpn/vpn_count.py" 2>/dev/null |
    python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read()); print(d["clients"] if d.get("ok") else "?")' 2>/dev/null || echo "?"
}

echo "== Nightwatch VPN cutover: pool $OLD_NET -> $NEW_NET, max-clients -> $MAX_CLIENTS"
echo "   now: $(grep -E '^\s*server\s' "$CONF") | $(grep -E '^\s*max-clients' "$CONF" || echo 'max-clients (unset)')"
echo "   firewall scripts: $(grep -c "$OLD_NET" "$FW" "$FWRM" | tr '\n' ' ')(lines with $OLD_NET)"
echo "   status file drop-in: $([ -f "$DROPIN" ] && echo present || echo 'missing (will be added)')"
echo "   players connected: $(clients)"
if [ "$MODE" = check ]; then
  echo "-- check only: nothing changed"
  exit 0
fi
if [ "$MODE" = run ]; then
  printf "Restart the VPN now? Everyone drops for ~30 s. Type yes: "
  read -r ans
  [ "$ans" = yes ] || die "not confirmed"
fi

# ---- backup ------------------------------------------------------------------------------------------
TS=$(date +%Y%m%d-%H%M%S)
B=$BK/$TS
mkdir -p "$B"
cp -a "$CONF" "$FW" "$FWRM" "$B/"
[ -f /etc/nftables.conf ] && cp -a /etc/nftables.conf "$B/"
[ -d "$CCD" ] && cp -a "$CCD" "$B/ccd"
[ -f "$DROPIN" ] && cp -a "$DROPIN" "$B/"
[ -f /opt/nwlobby/nwlobby.py ] && cp -a /opt/nwlobby/nwlobby.py /etc/systemd/system/nwlobby.service "$B/" 2>/dev/null || true
ln -sfn "$B" "$BK/latest"
echo "backup: $B"
restore_and_die() {
  echo "cutover: $1: restoring the backup" >&2
  "$FWRM" >/dev/null 2>&1 || true
  cp -a "$B/server.conf" "$CONF"; cp -a "$B/$(basename "$FW")" "$FW"; cp -a "$B/$(basename "$FWRM")" "$FWRM"
  "$FW" >/dev/null 2>&1 || true
  exit 1
}

# ---- firewall: old rules out (with the OLD script), /22 in, re-apply ---------------------------------------
"$FWRM" >/dev/null 2>&1 || true
sed -i "s|$OLD_NET|$NEW_NET|g" "$FW" "$FWRM"
grep -q "VPN=$NEW_NET" "$FW" || restore_and_die "firewall script edit failed"
"$FW" || restore_and_die "the new firewall rules did not apply"
# nftables (policy drop) matches interfaces, not addresses: nothing to widen there

# ---- server.conf: pool + max-clients ---------------------------------------------------------------------
sed -i -E "s|^(\s*server\s+10\.8\.0\.0\s+)255\.255\.255\.0|\1$NEW_MASK|" "$CONF"
if grep -qE '^\s*max-clients' "$CONF"; then
  sed -i -E "s|^(\s*max-clients\s+)[0-9]+|\1$MAX_CLIENTS|" "$CONF"
else
  echo "max-clients $MAX_CLIENTS" >> "$CONF"
fi
grep -qE "^\s*server\s+10\.8\.0\.0\s+$NEW_MASK" "$CONF" || restore_and_die "server.conf edit failed"
# a client-config-dir file with a pushed /24 netmask (not active unless client-config-dir is set) follows
[ -d "$CCD" ] && sed -i "s|255\.255\.255\.0|$NEW_MASK|g" "$CCD"/* 2>/dev/null || true

# ---- systemd drop-in: no --status file (Debian's unit passes it itself) -------------------------------------
install -d -m 755 "$DROPIN_DIR"
cat > "$DROPIN" <<'U'
# Nightwatch: no --status file (it held every client's real address). See tools/vpn/server/no-status-file.sh.
[Service]
ExecStart=
ExecStart=/usr/sbin/openvpn --suppress-timestamps --config %i.conf
U
systemctl daemon-reload

# ---- restart ---------------------------------------------------------------------------------------------
echo "restarting OpenVPN..."
systemctl restart "$UNIT"
rm -f /run/openvpn-server/status-*.log
OK=0
for i in $(seq 1 15); do
  sleep 1
  if systemctl is-active --quiet "$UNIT" && ip -4 addr show tun0 2>/dev/null | grep -q "10.8.0.1/22"; then OK=1; break; fi
done
[ $OK = 1 ] || { journalctl -u "$UNIT" -n 30 --no-pager >&2; die "OpenVPN is not up with 10.8.0.1/22: run rollback.sh"; }

# ---- nwlobby (/22 code + unit; it waits for 10.8.0.1 itself) ---------------------------------------------------
sh "$LOBBY_SRC/install.sh" >/dev/null || die "nwlobby install failed (VPN itself is up): check 'systemctl status nwlobby'"

# ---- checks ----------------------------------------------------------------------------------------------
echo
echo "== checks"
echo "   OpenVPN: $(systemctl is-active $UNIT), tun0 $(ip -4 -br addr show tun0 | awk '{print $3}')"
echo "   status file: $(ls /run/openvpn-server/status-*.log 2>/dev/null || echo none)"
echo "   nwlobby: $(systemctl is-active nwlobby) $(ss -H -lnu | grep -c '10.8.0.1:9310') socket(s); counts $(ss -H -lnu | grep -c '127.0.0.1:9311')"
echo "   firewall: $(iptables -S 2>/dev/null | grep -c "$NEW_NET") rule(s) with $NEW_NET, $(iptables -S 2>/dev/null | grep -c "$OLD_NET") with $OLD_NET"
echo "   management: $(ss -H -lnt | grep -c '127.0.0.1:7505') listener(s); players reconnecting: $(clients) (give it ~30 s)"
echo
echo "Done. Iosefka picks up pool 1021 / max-clients $MAX_CLIENTS at its next sample (5 min). Undo: sudo sh $SRC/rollback.sh"
