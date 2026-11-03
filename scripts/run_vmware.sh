#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VMX_PATH="$ROOT/image/starlix.vmx"
LOG_PATH="$ROOT/image/serial.log"

# Clear previous serial log
> "$LOG_PATH"

echo "============================================================"
echo " STARLIX OS - VMWARE AUTOMATED LAUNCHER & DEBUG MONITOR"
echo "============================================================"

# Find VMware Binary
VM_BIN=""
if command -v vmplayer >/dev/null 2>&1; then
    VM_BIN="vmplayer"
elif command -v vmware >/dev/null 2>&1; then
    VM_BIN="vmware"
elif [ -f "/usr/bin/vmplayer" ]; then
    VM_BIN="/usr/bin/vmplayer"
elif [ -f "/usr/bin/vmware" ]; then
    VM_BIN="/usr/bin/vmware"
fi

if [ -z "$VM_BIN" ]; then
    echo "[-] VMware Player / Workstation executable not found in PATH!"
    echo "[-] Please ensure VMware is installed on your Ubuntu system."
    exit 1
fi

echo "[+] Found VMware Executable: $VM_BIN"
echo "[*] Launching $VMX_PATH in VMware..."

# Launch VMware in background
"$VM_BIN" "$VMX_PATH" > /dev/null 2>&1 &
VM_PID=$!

echo "[+] VMware Launched (PID: $VM_PID)"
echo "[*] Streaming Real-Time Serial Debug Logs (Ctrl+C to stop log tail):"
echo "------------------------------------------------------------"

sleep 2
# Stream Serial Debug Log
tail -f "$LOG_PATH"
