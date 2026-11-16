# Build a SteamOS test VM in VirtualBox 7.1 (no extension pack): SATA only, VBoxSVGA (no vmwgfx in the SteamOS kernel).
# Recipe follows i12bretro "Running a SteamOS VM in VirtualBox". Software Vulkan only; tests start/compat, not performance.
# Phase A (this script): create VM with empty target disk (SATA 0) + recovery stick (SATA 1).
# Phase B (in the live recovery, over ssh, see docs in research/steamos_vm.md): patch ~/tools/repair_device.sh
#   (DISK=/dev/sda, DISK_SUFFIX= blank, sanitize/bios/controller steps -> no-ops), run it with NOPROMPT=1 via systemd-run
#   (a plain nohup child dies with the ssh session), then for partset A and B: steamos-chroot -> passwd, sshd enable,
#   /etc/sddm.conf.d/zz-steamos-desktopmode.conf [Autologin] Session=plasma.desktop (gamescope Game Mode aborts without a GPU).
# Phase C: poweroff, detach SATA 1 (recovery). NEVER boot the live stick while the installed disk is attached: the live
#   image's steamos-bootconf writes boot-ok into the installed ESP (same partlabels) and steamcl then hangs before the kernel.
param([string]$Root = "E:\games\bbport\agents\steamvm082", [string]$Name = "steamos082",
      [string]$Image = "steamdeck-oobe-repair-20260707.10-3.8.14.img", [int]$DiskMB = 122880)
$vb = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$dl = Join-Path $Root "dl"; $vm = Join-Path $Root "vm"
New-Item -ItemType Directory -Force $dl, $vm | Out-Null
$bz = Join-Path $dl "$Image.bz2"
if (-not (Test-Path $bz)) { curl.exe -L -o $bz "https://steamdeck-images.steamos.cloud/recovery/$Image.bz2" }  # no published checksum; record ours
$raw = Join-Path $dl $Image
if (-not (Test-Path $raw)) { bzip2 -dk $bz }
$stick = Join-Path $vm "recovery.vdi"
if (-not (Test-Path $stick)) { & $vb convertfromraw $raw $stick --format VDI }
& $vb createvm --name $Name --ostype ArchLinux_64 --register --basefolder $vm
& $vb modifyvm $Name --firmware efi64 --cpus 4 --memory 8192 --vram 128 --graphicscontroller vboxsvga --accelerate-3d off `
   --nic1 nat --natpf1 "ssh,tcp,127.0.0.1,2222,,22" --audio-driver none --usb-xhci off --usb-ehci off --usb-ohci off `
   --keyboard ps2 --mouse ps2 --rtc-use-utc on
& $vb storagectl $Name --name SATA --add sata --controller IntelAhci --portcount 2
$disk = Join-Path $vm "$Name.vdi"
if (-not (Test-Path $disk)) { & $vb createmedium disk --filename $disk --size $DiskMB --format VDI --variant Standard }
& $vb storageattach $Name --storagectl SATA --port 0 --device 0 --type hdd --medium $disk
& $vb storageattach $Name --storagectl SATA --port 1 --device 0 --type hdd --medium $stick
Write-Host "created $Name"
