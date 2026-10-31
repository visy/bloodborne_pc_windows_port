#!/bin/sh
# OpenVPN DCO (data channel offload) for the Nightwatch VPN server: the kernel carries the tunnel's packets on every core
# instead of the single OpenVPN thread (one core). Same instance, port, pool and profile: no change for players.
# OpenVPN 2.7.8 on the server is built with [DCO]; Debian 13 ships openvpn-dco-dkms (module ovpn-dco-v2).
#
#   sudo sh dco.sh --check     what is ready / missing, config lint (nothing changes)
#   sudo sh dco.sh prep        install kernel headers + dkms + openvpn-dco-dkms, build and load the module.
#                              NO OpenVPN restart: the running VPN is untouched (it keeps its userspace data path).
#   sudo sh dco.sh enable      restart OpenVPN once so it opens tun0 as a DCO device (everyone reconnects, ~30-40 s;
#                              announce first: iosefka vpn-maint). Asks to confirm; refuses if the module is not loaded.
#   sudo sh dco.sh disable     restart OpenVPN with 'disable-dco' (back to the userspace data path, module kept)
#   sudo sh dco.sh reboot      when the module could only be built for a NEWER installed kernel (the running kernel's
#                              headers left the archive): drop the hold and reboot into it with DCO on. One downtime
#                              (~1-2 min) that also applies the pending kernel security update. Announce first.
#
# After enable: `ip -d link show tun0` names ovpn-dco; per-core load spreads (top shows ksoftirqd/N, OpenVPN itself idle).
set -eu
CONF=/etc/openvpn/server/server.conf
UNIT=openvpn-server@server.service
K=$(uname -r)
# the newest installed kernel image (unattended upgrades install new kernels; the running one may be older)
NEWEST=$(ls /boot/vmlinuz-* 2>/dev/null | sed 's|/boot/vmlinuz-||' | sort -V | tail -1)
MODE=${1:---check}
die() { echo "dco: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"

module_loaded() { grep -q '^ovpn_dco_v2 ' /proc/modules 2>/dev/null || grep -q '^ovpn ' /proc/modules 2>/dev/null; }
tun_is_dco() { ip -d link show tun0 2>/dev/null | grep -q 'ovpn'; }
clients() {
  HOME_OP=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
  NWVPN_PW="$HOME_OP/nwvpn/mgmt.pw" python3 -I "$HOME_OP/nwvpn/vpn_count.py" 2>/dev/null |
    python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read()); print(d["clients"] if d.get("ok") else "?")' 2>/dev/null || echo "?"
}
lint() {
  # DCO needs an AEAD data cipher and none of: compression, fragment, static key, a disable-dco line.
  bad=$(grep -nE '^\s*(compress|comp-lzo|fragment|secret)\b' "$CONF" || true)
  [ -z "$bad" ] && echo "   config: ok for DCO (no compression / fragment / static key)" || { echo "   config: NOT ok for DCO:"; echo "$bad" | sed 's/^/     /'; }
  grep -qE '^\s*disable-dco' "$CONF" && echo "   held off: disable-dco is set (prep put it there; 'enable' removes it)" || true
  grep -qE '^\s*data-ciphers\s+.*(AES-(128|256)-GCM|CHACHA20-POLY1305)' "$CONF" && echo "   data ciphers: AEAD" || echo "   data ciphers: check (DCO needs AES-GCM or CHACHA20-POLY1305)"
}
status() {
  echo "== DCO status (kernel $K)"
  echo "   OpenVPN: $(/usr/sbin/openvpn --version 2>/dev/null | head -1 | grep -o '\[DCO\]' || echo 'built WITHOUT DCO')"
  echo "   headers for the running $K: $([ -d /lib/modules/$K/build ] && echo installed || echo missing)"
  [ "$NEWEST" != "$K" ] && echo "   newest installed kernel: $NEWEST (reboot pending), headers $([ -d /lib/modules/$NEWEST/build ] && echo installed || echo missing)"
  echo "   dkms module: $(dkms status 2>/dev/null | grep -i ovpn || echo 'not installed')"
  echo "   module loaded: $(module_loaded && echo yes || echo no)"
  echo "   tun0 is a DCO device: $(tun_is_dco && echo YES || echo 'no (userspace data path)')"
  echo "   secure boot: $([ -d /sys/firmware/efi ] && echo 'EFI: check mokutil --sb-state' || echo 'no EFI: unsigned modules load')"
  lint
  echo "   players connected: $(clients)"
}

case "$MODE" in
  --check) status ;;
  prep)
    status
    echo "== prep: packages and module (no VPN restart)"
    apt-get update -qq
    # headers for the running kernel when the archive still has them, always the metapackage (the newest kernel and
    # future ones); dkms builds the module for every kernel that has headers
    HDR="linux-headers-cloud-amd64"
    apt-cache show "linux-headers-$K" >/dev/null 2>&1 && HDR="$HDR linux-headers-$K"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q $HDR dkms openvpn-dco-dkms || die "package install failed"
    echo "ovpn-dco-v2" > /etc/modules-load.d/ovpn-dco.conf   # loaded at boot
    if [ -d "/lib/modules/$K/build" ]; then
      modprobe ovpn-dco-v2 2>/dev/null || modprobe ovpn 2>/dev/null || die "the module did not load: dkms status; journalctl -k | tail"
      module_loaded || die "module not loaded after modprobe"
    else
      dkms status 2>/dev/null | grep -i ovpn | grep -q "$NEWEST" || die "the module was not built for $NEWEST: dkms status"
      echo "The running kernel $K has no headers in the archive any more: the module is built for $NEWEST"
      echo "(already installed, waiting for a reboot) and loads after the reboot into it: sudo sh $0 reboot (announce first)."
    fi
    # with the module loaded, ANY later OpenVPN start (a reboot, a rollback) would turn DCO on unasked: keep it off in the
    # config until `enable` removes this line (read only at start, so the running VPN is unaffected)
    grep -qE '^\s*disable-dco' "$CONF" || { cp -a "$CONF" "$CONF.pre-dco-prep"; echo "disable-dco   # nightwatch: dco.sh enable removes this" >> "$CONF"; }
    echo
    status
    echo "Prep done. The VPN was not restarted and runs without DCO until: sudo sh $0 enable (or reboot, see above)"
    ;;
  enable)
    module_loaded || die "the DCO module is not loaded: run 'sudo sh $0 prep' first"
    grep -qE '^\s*disable-dco' "$CONF" && { cp -a "$CONF" "$CONF.pre-dco"; sed -i '/^\s*disable-dco/d' "$CONF"; }
    echo "Players connected: $(clients). This restarts OpenVPN once (~30-40 s reconnect for everyone)."
    printf "Type yes to restart now: "; read -r ans; [ "$ans" = yes ] || die "not confirmed"
    systemctl restart "$UNIT"
    for i in $(seq 1 15); do sleep 1; systemctl is-active --quiet "$UNIT" && ip -4 addr show tun0 2>/dev/null | grep -q 10.8.0.1 && break; done
    systemctl is-active --quiet "$UNIT" || { journalctl -u "$UNIT" -n 30 --no-pager >&2; die "OpenVPN is not up: sudo sh $0 disable"; }
    if tun_is_dco; then echo "OpenVPN runs with DCO: $(ip -d link show tun0 | grep -o 'ovpn[a-z_-]*' | head -1)"
    else echo "OpenVPN is up but tun0 is NOT a DCO device (it fell back to userspace; players are fine). journalctl -u $UNIT | grep -i dco" >&2; fi
    sleep 20; echo "players reconnecting: $(clients)"
    ;;
  reboot)
    [ "$NEWEST" != "$K" ] || die "no newer kernel is installed: use 'enable' (a restart only)"
    dkms status 2>/dev/null | grep -i ovpn | grep -q "$NEWEST" || die "no DCO module built for $NEWEST: run 'prep' first"
    echo "Players connected: $(clients). This REBOOTS the VPN server into $NEWEST with DCO on (~1-2 min for everyone)."
    printf "Type yes to reboot now: "; read -r ans; [ "$ans" = yes ] || die "not confirmed"
    grep -qE '^\s*disable-dco' "$CONF" && { cp -a "$CONF" "$CONF.pre-dco-reboot"; sed -i '/^\s*disable-dco/d' "$CONF"; }
    echo "Rebooting. Afterwards: sudo sh $0 --check (tun0 should be a DCO device)."
    systemctl reboot
    ;;
  disable)
    grep -qE '^\s*disable-dco' "$CONF" || { cp -a "$CONF" "$CONF.pre-nodco"; echo "disable-dco" >> "$CONF"; }
    echo "Players connected: $(clients). This restarts OpenVPN once."
    printf "Type yes to restart now: "; read -r ans; [ "$ans" = yes ] || die "not confirmed"
    systemctl restart "$UNIT"; sleep 5
    echo "OpenVPN: $(systemctl is-active $UNIT), DCO: $(tun_is_dco && echo still on || echo off)"
    ;;
  *) die "usage: sudo sh dco.sh --check | prep | enable | reboot | disable" ;;
esac
