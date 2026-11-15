#!/usr/bin/env bash
# Idempotent setup of Proton-GE + umu-launcher + software Vulkan for testing Windows builds under Proton
# on a Linux box (written for WSL2 Ubuntu without a GPU Vulkan driver). No sudo is used by this script:
# the apt packages it checks for are listed at the end if missing.
#   usage: proton_wsl_setup.sh [root]      (default root: ~/proton082)
set -euo pipefail
ROOT=${1:-$HOME/proton082}
GE_TAG=${GE_TAG:-GE-Proton11-12}
UMU_VER=${UMU_VER:-1.4.4}
DL=$ROOT/dl
mkdir -p "$DL/ge" "$DL/umu" "$ROOT/proton" "$ROOT/umu" "$ROOT/prefix" "$ROOT/runs"

# Downloads are untrusted: each in its own directory, nothing is run from there except umu's documented zipapp.
if [[ ! -f $DL/ge/$GE_TAG-x86_64.tar.gz ]]; then
  u=https://github.com/GloriousEggroll/proton-ge-custom/releases/download/$GE_TAG
  (cd "$DL/ge" && curl -fsSLO "$u/$GE_TAG-x86_64.tar.gz" && curl -fsSLO "$u/$GE_TAG-x86_64.sha512sum")
fi
(cd "$DL/ge" && sha512sum -c "$GE_TAG-x86_64.sha512sum")
[[ -f $ROOT/proton/$GE_TAG-x86_64/toolmanifest.vdf || -f $ROOT/proton/$GE_TAG/toolmanifest.vdf ]] || tar -xzf "$DL/ge/$GE_TAG-x86_64.tar.gz" -C "$ROOT/proton"

if [[ ! -f $DL/umu/umu-launcher-$UMU_VER-zipapp.tar ]]; then
  (cd "$DL/umu" && curl -fsSLO "https://github.com/Open-Wine-Components/umu-launcher/releases/download/$UMU_VER/umu-launcher-$UMU_VER-zipapp.tar")
fi
[[ -e $ROOT/umu/umu/umu-run ]] || tar -xf "$DL/umu/umu-launcher-$UMU_VER-zipapp.tar" -C "$ROOT/umu"

# Software Vulkan (lavapipe) and the loader; display comes from WSLg (DISPLAY=:0 XWayland).
miss=()
for p in libvulkan1 mesa-vulkan-drivers libsdl2-2.0-0 libgl1; do dpkg -s "$p" >/dev/null 2>&1 || miss+=("$p"); done
[[ -f /usr/share/vulkan/icd.d/lvp_icd.x86_64.json || -f /usr/share/vulkan/icd.d/lvp_icd.json ]] || miss+=(mesa-vulkan-drivers)
if ((${#miss[@]})); then echo "MISSING apt packages (needs sudo): sudo apt install ${miss[*]}"; fi
echo "proton: $ROOT/proton/$GE_TAG-x86_64 (PROTONPATH)"
echo "umu:    $(ls -d "$ROOT"/umu/*/umu-run 2>/dev/null || echo "$ROOT/umu (see tar layout)")"

# Newer lavapipe without root (DXVK 3 needs VK_KHR_load_store_op_none, absent from Mesa 23.2): the noble-updates
# Mesa 25.2 Vulkan drivers and LLVM 20 unpacked with dpkg-deb into $ROOT/mesa25. Use it with
#   VK_DRIVER_FILES=$ROOT/mesa25/lvp_icd.json EXTRA_ENV="LD_LIBRARY_PATH=$ROOT/mesa25/usr/lib/x86_64-linux-gnu"
if [[ ! -f $ROOT/mesa25/lvp_icd.json ]]; then
  mkdir -p "$DL/mesa25"
  P=http://archive.ubuntu.com/ubuntu/pool/main
  (cd "$DL/mesa25"     && curl -fsSLO $P/m/mesa/mesa-vulkan-drivers_25.2.8-0ubuntu0.24.04.5_amd64.deb     && curl -fsSLO $P/l/llvm-toolchain-20/libllvm20_20.1.2-0ubuntu1~24.04.3_amd64.deb)
  mkdir -p "$ROOT/mesa25"
  for f in "$DL"/mesa25/*.deb; do dpkg-deb -x "$f" "$ROOT/mesa25"; done
  M=$ROOT/mesa25/usr/lib/x86_64-linux-gnu
  printf '{"ICD":{"api_version":"1.4.318","library_path":"%s/libvulkan_lvp.so"},"file_format_version":"1.0.0"}
' "$M" > "$ROOT/mesa25/lvp_icd.json"
fi
echo "lavapipe 25: $ROOT/mesa25/lvp_icd.json"
# vm.max_map_count below 1048576 (Steam Deck / SteamOS sets it high) triggers Proton's warning; raising it needs root:
#   sudo sysctl -w vm.max_map_count=1048576
cat /proc/sys/vm/max_map_count
