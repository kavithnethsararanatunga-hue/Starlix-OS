#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$ROOT/image/starlix.img"
VDI="$ROOT/image/starlix.vdi"
SERIAL_LOG="$ROOT/image/serial.log"

echo "============================================================"
echo " LAUNCHING STARLIX OS IN VIRTUALBOX (EFI + USB HARDWARE)"
echo "============================================================"

# Kill previous instances
pkill -9 -f VirtualBoxVM 2>/dev/null || true
sleep 1

# Unregister VM & Reset Serial Log
VBoxManage unregistervm "StarlixOS" --delete 2>/dev/null || true
VBoxManage closemedium disk "$VDI" 2>/dev/null || true
rm -f "$VDI" "$SERIAL_LOG"
rm -rf "$HOME/VirtualBox VMs/StarlixOS" 2>/dev/null || true

touch "$SERIAL_LOG"

# Convert RAW image -> VDI
echo "[*] Converting Disk Image to VDI..."
VBoxManage convertfromraw "$IMG" "$VDI" --format VDI > /dev/null

# Create fresh VM with USB 2.0 EHCI & USB 3.0 xHCI
echo "[*] Registering VirtualBox VM with USB Controllers..."
VBoxManage createvm --name "StarlixOS" --ostype "Linux_64" --register
VBoxManage modifyvm "StarlixOS" --firmware efi --memory 512 --cpus 2
VBoxManage usbcontroller add "StarlixOS" --name "EHCI" --type ehci 2>/dev/null || VBoxManage modifyvm "StarlixOS" --usbehci on 2>/dev/null || true
VBoxManage usbcontroller add "StarlixOS" --name "xHCI" --type xhci 2>/dev/null || VBoxManage modifyvm "StarlixOS" --usbxhci on 2>/dev/null || true

# Configure Serial Port 1 (COM1) -> Pipe output to image/serial.log
VBoxManage modifyvm "StarlixOS" --uart1 0x3F8 4 --uartmode1 file "$SERIAL_LOG"

# Attach Storage
VBoxManage storagectl "StarlixOS" --name "SATA Controller" --add sata --controller IntelAhci
VBoxManage storageattach "StarlixOS" --storagectl "SATA Controller" --port 0 --device 0 --type hdd --medium "$VDI"

# Launch VirtualBox VM in background
VBoxManage startvm "StarlixOS" --type gui &

echo ""
echo "[+] VirtualBox Started!"
echo "[*] STREAMING REAL-TIME SERIAL DEBUG LOGS TO TERMINAL BELOW:"
echo "------------------------------------------------------------"

sleep 2
tail -f "$SERIAL_LOG"
