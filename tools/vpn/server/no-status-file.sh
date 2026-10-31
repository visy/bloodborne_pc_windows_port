#!/bin/sh
# Stop OpenVPN writing a status file on the Nightwatch VPN server. Run as root:   sudo sh no-status-file.sh
# Undo:   sudo sh no-status-file.sh --undo
# Both restart OpenVPN, so they refuse while players are connected (count from ~$SUDO_USER/nwvpn/vpn_count.py);
# add --now to restart anyway.
#
# Debian's openvpn-server@.service starts OpenVPN with `--status /run/openvpn-server/status-%i.log` itself, so a
# status file holding every connected client's real address exists even with no status line in server.conf
# (found 2026-10-31). This drop-in replaces the unit's ExecStart with the same command minus --status.
# The player count does not need it (vpn_count.py asks the management interface for load-stats).
# OpenVPN restarts once: connected players drop and reconnect by themselves within about 30 s.
set -eu
UNIT=openvpn-server@server.service
DIR=/etc/systemd/system/$UNIT.d
F=$DIR/nightwatch-no-status.conf
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
case " $* " in *" --now "*) NOW=1 ;; *) NOW=0 ;; esac
if [ "$NOW" = 0 ]; then
  HOME_OP=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
  N=$(NWVPN_PW="$HOME_OP/nwvpn/mgmt.pw" python3 -I "$HOME_OP/nwvpn/vpn_count.py" 2>/dev/null |
      python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read()); print(d["clients"] if d.get("ok") else "?")' 2>/dev/null || echo "?")
  if [ "$N" != 0 ]; then
    echo "not now: $N player(s) connected (or the count failed); OpenVPN would restart. Try when it is 0, or add --now." >&2
    exit 2
  fi
fi
if case " $* " in *" --undo "*) true ;; *) false ;; esac; then
  rm -f "$F"; rmdir "$DIR" 2>/dev/null || true
  systemctl daemon-reload; systemctl restart "$UNIT"
  echo "status file back (Debian's default unit)"; exit 0
fi
install -d -m 755 "$DIR"
cat > "$F" <<'U'
# Nightwatch: no --status file (it held every client's real address). See tools/vpn/server/no-status-file.sh.
[Service]
ExecStart=
ExecStart=/usr/sbin/openvpn --suppress-timestamps --config %i.conf
U
systemctl daemon-reload
systemctl restart "$UNIT"
rm -f /run/openvpn-server/status-*.log
sleep 3
systemctl is-active --quiet "$UNIT" || { journalctl -u "$UNIT" -n 20 --no-pager >&2; echo "OpenVPN did not start; undo with --undo" >&2; exit 1; }
ls /run/openvpn-server/
echo "OpenVPN runs without a status file."
