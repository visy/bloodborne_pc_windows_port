#!/bin/sh
# nwsign installer for the Nightwatch VPN server (Debian 13). Run as root from a copy of this directory:
#     sudo NWSIGN_VPN_HOST=<vpn-host> sh install.sh
# Idempotent: safe to run again (keeps keys, state and config; refreshes code, unit and server.conf edits).
# Roll back with uninstall.sh (restores the previous server.conf, so the old shared tls-crypt profile works again).
set -eu

OVPN=/etc/openvpn/server
CONF=$OVPN/server.conf
BAK=$CONF.pre-nwsign
APP=/opt/nwsign
STATE=/var/lib/nwsign
ETC=/etc/nwsign
PORT=${NWSIGN_PORT:-8443}
FW=/etc/iptables/add-openvpn-rules.sh
FWRM=/etc/iptables/rm-openvpn-rules.sh
SRC=$(cd "$(dirname "$0")" && pwd)

die() { echo "install.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
for f in nwsign_core.py nwsign_service.py nwsign-admin nwsign-verify nwsign.service; do
  [ -f "$SRC/$f" ] || die "missing $SRC/$f"
done
for f in "$CONF" "$OVPN/ca.crt"; do [ -f "$f" ] || die "missing $f"; done
# The root CA key and CRL that belong to ca.crt (a second OpenVPN setup can leave a stray ca.key / crl.pem
# in $OVPN; the easy-rsa PKI often holds the real pair). Override with OVPN_ROOT_KEY / OVPN_ROOT_CRL.
pubhash() { openssl "$@" 2>/dev/null | sha256sum | cut -c1-64; }
WANT=$(pubhash x509 -in "$OVPN/ca.crt" -noout -pubkey)
ROOT_KEY=${OVPN_ROOT_KEY:-}
if [ -z "$ROOT_KEY" ]; then
  for k in "$OVPN/ca.key" /etc/openvpn/easy-rsa/pki/private/ca.key; do
    [ -f "$k" ] && [ "$(pubhash pkey -in "$k" -pubout)" = "$WANT" ] && { ROOT_KEY=$k; break; }
  done
fi
[ -n "$ROOT_KEY" ] && [ "$(pubhash pkey -in "$ROOT_KEY" -pubout)" = "$WANT" ] || die "no CA key matching $OVPN/ca.crt (set OVPN_ROOT_KEY=<path>)"
ROOT_CRL=${OVPN_ROOT_CRL:-}
if [ -z "$ROOT_CRL" ]; then
  for c in "$OVPN/crl.pem" /etc/openvpn/easy-rsa/pki/crl.pem; do
    [ -f "$c" ] && openssl crl -in "$c" -CAfile "$OVPN/ca.crt" -noout 2>/dev/null && { ROOT_CRL=$c; break; }
  done
fi
crl_days_left() { n=$(openssl crl -in "$1" -noout -nextupdate | cut -d= -f2); echo $(( ($(date -d "$n" +%s) - $(date +%s)) / 86400 )); }
if [ -z "$ROOT_CRL" ] || ! openssl crl -in "$ROOT_CRL" -CAfile "$OVPN/ca.crt" -noout 2>/dev/null \
   || [ "$(crl_days_left "$ROOT_CRL")" -lt 60 ]; then
  # No usable root CRL: issue an empty one (10 years) with the root key. It revokes nothing (the old shared
  # profile stops working anyway: the server switches to tls-crypt-v2).
  T=$(mktemp -d); touch "$T/idx"; echo 01 > "$T/num"
  printf '[ca]\ndefault_ca=c\n[c]\ndatabase=%s/idx\ncrlnumber=%s/num\ncertificate=%s\nprivate_key=%s\ndefault_md=sha256\n' \
    "$T" "$T" "$OVPN/ca.crt" "$ROOT_KEY" > "$T/c.cnf"
  openssl ca -gencrl -config "$T/c.cnf" -crldays 3650 -out "$OVPN/root-crl-nwsign.pem" 2>/dev/null || die "could not issue a root CRL"
  rm -rf "$T"; chmod 644 "$OVPN/root-crl-nwsign.pem"; ROOT_CRL=$OVPN/root-crl-nwsign.pem
fi
NEXT=$(openssl crl -in "$ROOT_CRL" -noout -nextupdate | cut -d= -f2)
echo "root CA key: $ROOT_KEY; root CRL: $ROOT_CRL (next update $NEXT)"
command -v openssl >/dev/null && command -v python3 >/dev/null || die "need openssl and python3"
OPENVPN=/usr/sbin/openvpn
[ -x $OPENVPN ] || die "$OPENVPN not found"

# ---- config (the VPN host lives only here, never in the repo) ---------------------------------
mkdir -p "$ETC"
if [ ! -f "$ETC/nwsign.json" ]; then
  [ -n "${NWSIGN_VPN_HOST:-}" ] || die "set NWSIGN_VPN_HOST=<the public name or address clients connect to>"
  NWSIGN_VPN_HOST=$NWSIGN_VPN_HOST PORT=$PORT python3 - <<'PY'
import json, os
json.dump({"vpn_host": os.environ["NWSIGN_VPN_HOST"], "listen_port": int(os.environ["PORT"])},
          open("/etc/nwsign/nwsign.json", "w"), indent=2)
PY
fi

# ---- user, code ------------------------------------------------------------------------------
id nwsign >/dev/null 2>&1 || useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin --user-group nwsign
chown root:nwsign "$ETC" "$ETC/nwsign.json"; chmod 750 "$ETC"; chmod 640 "$ETC/nwsign.json"
install -d -m 755 -o root -g root "$APP"
for f in nwsign_core.py nwsign_service.py nwsign-admin nwsign-verify; do
  install -m 755 -o root -g root "$SRC/$f" "$APP/$f"
done
install -d -m 755 -o nwsign -g nwsign "$STATE"
install -d -m 700 -o nwsign -g nwsign "$STATE/priv"
install -d -m 755 -o nwsign -g nwsign "$STATE/pub"
cat > /usr/local/sbin/nwsign-admin <<'W'
#!/bin/sh
exec runuser -u nwsign -- env NWSIGN_CONF=/etc/nwsign/nwsign.json /usr/bin/python3 -I /opt/nwsign/nwsign-admin "$@"
W
chmod 755 /usr/local/sbin/nwsign-admin
ADMIN="runuser -u nwsign -- env NWSIGN_CONF=$ETC/nwsign.json python3 -I $APP/nwsign-admin"

# ---- sub-CA + HTTPS key (as nwsign); sub-CA signed once by the root key (as root) --------------
PIN=$($ADMIN init)
# (re)sign unless a sub-CA cert issued by this root is there (a failed earlier run can leave an empty file)
if ! openssl verify -CAfile "$OVPN/ca.crt" "$STATE/priv/subca.crt" >/dev/null 2>&1; then
  NWSIGN_CONF=$ETC/nwsign.json python3 -I "$APP/nwsign-admin" sign-subca --root-key "$ROOT_KEY" --root-crt "$OVPN/ca.crt"
fi
cp "$OVPN/ca.crt" "$STATE/priv/root-ca.crt"
cp "$ROOT_CRL" "$STATE/priv/root-crl.pem"
if [ ! -f "$STATE/priv/tc2-server.key" ]; then
  if [ ! -f "$OVPN/tls-crypt-v2.key" ]; then
    $OPENVPN --genkey tls-crypt-v2-server "$OVPN/tls-crypt-v2.key"; chmod 600 "$OVPN/tls-crypt-v2.key"
  fi
  cp "$OVPN/tls-crypt-v2.key" "$STATE/priv/tc2-server.key"
fi
grep -q 'tls-crypt-v2 server key' "$STATE/priv/tc2-server.key" || die "tls-crypt-v2.key is not a tls-crypt-v2 SERVER key"
chown -R nwsign:nwsign "$STATE/priv"; chmod 700 "$STATE/priv"; chmod 600 "$STATE"/priv/*.key
cat "$OVPN/ca.crt" "$STATE/priv/subca.crt" > "$OVPN/ca-bundle.crt"; chmod 644 "$OVPN/ca-bundle.crt"
$ADMIN regen-crl

# ---- firewall: open the signing port (persisted through the existing rules script) -------------
if [ -f "$FW" ] && ! grep -q "dport $PORT " "$FW"; then
  cp -an "$FW" "$FW.pre-nwsign"; cp -an "$FWRM" "$FWRM.pre-nwsign"
  sed -i "/--dport 1194 -j ACCEPT/a iptables -I INPUT 1 -i eth0 -p tcp --dport $PORT -j ACCEPT   # nwsign" "$FW"
  sed -i "/--dport 1194 -j ACCEPT/a iptables -D INPUT -i eth0 -p tcp --dport $PORT -j ACCEPT 2>/dev/null   # nwsign" "$FWRM"
fi
iptables -C INPUT -i eth0 -p tcp --dport "$PORT" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -i eth0 -p tcp --dport "$PORT" -j ACCEPT
# A separate nftables table (/etc/nftables.conf, "inet filter", input policy drop) also sees every packet:
# open the port there too, live and in the file (next to the 1194 rule).
NFT=/etc/nftables.conf
if command -v nft >/dev/null && nft list chain inet filter input >/dev/null 2>&1; then
  nft list chain inet filter input | grep -q "tcp dport $PORT accept" || nft add rule inet filter input tcp dport "$PORT" accept
  if [ -f "$NFT" ] && ! grep -q "tcp dport $PORT accept" "$NFT"; then
    cp -an "$NFT" "$NFT.pre-nwsign"
    sed -i "/^[[:space:]]*udp dport 1194 accept/a\        tcp dport $PORT accept        # nwsign" "$NFT"
    nft -c -f "$NFT" || { cp -a "$NFT.pre-nwsign" "$NFT"; die "nftables.conf edit did not check; restored"; }
  fi
fi

# ---- service -------------------------------------------------------------------------------------
install -m 644 "$SRC/nwsign.service" /etc/systemd/system/nwsign.service
systemctl daemon-reload
systemctl enable nwsign.service >/dev/null 2>&1
systemctl restart nwsign.service
OK=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
  if python3 - "$PORT" <<'PY'
import ssl, sys, http.client
h = http.client.HTTPSConnection("127.0.0.1", int(sys.argv[1]), context=ssl._create_unverified_context(), timeout=3)
h.request("GET", "/v1/health")
sys.exit(0 if h.getresponse().status == 200 else 1)
PY
  then OK=1; break; fi
done
[ $OK = 1 ] || { journalctl -u nwsign -n 30 --no-pager >&2; die "nwsign did not come up; openvpn untouched"; }

# ---- openvpn: server.conf (backup once), then restart -------------------------------------------
[ -f "$BAK" ] || cp -a "$CONF" "$BAK"
python3 - "$CONF" <<'PY'
import re, sys
p = sys.argv[1]
out = []
for l in open(p).read().splitlines():
    s = l.strip()
    if s == "# --- nwsign ---":
        break                                   # drop a previous block; re-added below
    if re.match(r"(duplicate-cn|tls-crypt\s+tc\.key)\s*$", s):
        continue
    if re.match(r"ca\s+ca\.crt\s*$", s):
        l = "ca ca-bundle.crt"
    out.append(l)
out += ["# --- nwsign ---",
        "tls-crypt-v2 tls-crypt-v2.key",
        "verify-client-cert require",
        "crl-verify /var/lib/nwsign/pub/crl-bundle.pem",
        "script-security 2",
        "tls-crypt-v2-verify /opt/nwsign/nwsign-verify"]
open(p, "w").write("\n".join(out) + "\n")
PY
[ "$(grep -c '^script-security' "$CONF")" = 1 ] || die "server.conf has several script-security lines; fix by hand (backup: $BAK)"
systemctl restart openvpn-server@server
sleep 3
if ! systemctl is-active --quiet openvpn-server@server; then
  echo "openvpn failed to start with the new config; restoring the old one" >&2
  journalctl -u openvpn-server@server -n 20 --no-pager >&2 || true
  cp -a "$BAK" "$CONF"; systemctl restart openvpn-server@server
  die "rolled back server.conf"
fi

cat <<MSG

nwsign installed and openvpn restarted.
  HTTPS pin (SHA-256 of the SPKI; give this to the launcher):
    $PIN
  signing endpoint: https://<vpn-host>:$PORT/v1/enroll
  admin:            sudo nwsign-admin stats | list | ban <id> | unban <id>
  NOTE: the old shared tls-crypt profile no longer connects. Rollback: sh uninstall.sh
MSG
