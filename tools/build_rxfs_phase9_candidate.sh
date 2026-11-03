#!/usr/bin/env bash
# Builds a new isolated GPT VM candidate. Never writes image/ or any physical disk.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE="$ROOT/build/candidate_rxfs_phase9"
STAMP="$(date +%Y%m%d_%H%M%S)"
RUN="$BASE/run_$STAMP"
mkdir -p "$BASE"
mkdir "$RUN"  # unique run directory; abort if it already exists

for cmd in dd truncate mkfs.fat mmd mcopy sgdisk awk; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "MISSING TOOL: $cmd" >&2; exit 1; }
done

BOOTX64="$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI"
STBMGR="$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi"
RXFS_MKFS="$ROOT/RXFS/tools/mkfs.rxfs"
SSP_SOURCE="$ROOT/build/candidate_basic_graphics/vmtest_20261023_121437/starlix-basic-graphics-test.ssp.img"
for f in "$BOOTX64" "$STBMGR" "$RXFS_MKFS" "$SSP_SOURCE"; do
  [ -f "$f" ] || { echo "Required file missing: $f" >&2; exit 1; }
done

ESP_MB=64
RXFS_MB=64
SSP_MB=128
TOTAL_MB=$((ESP_MB + RXFS_MB + SSP_MB + 8))
ESP="$RUN/starlix-rxfs-phase9.esp.img"
RXFS="$RUN/starlix-rxfs-phase9.rxfs.img"
SSP="$RUN/starlix-rxfs-phase9.ssp.img"
DISK="$RUN/starlix-rxfs-phase9.img"
VMDK="$RUN/starlix-rxfs-phase9.vmdk"
VMX="$RUN/starlix-rxfs-phase9.vmx"

# Create a standalone FAT32 ESP and copy the CURRENT build outputs into it.
dd if=/dev/zero of="$ESP" bs=1M count="$ESP_MB" status=none
mkfs.fat -F 32 "$ESP" >/dev/null
for d in EFI EFI/BOOT EFI/OpenRPX EFI/OpenRPX/xsOft EFI/OpenRPX/xsOft/Boot; do
  mmd -i "$ESP" "::$d"
done
mcopy -i "$ESP" "$BOOTX64" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$STBMGR" ::/EFI/OpenRPX/xsOft/Boot/stbtmngr.efi

# Format a throwaway RXFS image. It intentionally has no BaseSystem.dmg;
# this run tests UEFI partition discovery/mount and observes the fallback.
truncate -s "$((RXFS_MB * 1024 * 1024))" "$RXFS"
"$RXFS_MKFS" "$RXFS" --size "$((RXFS_MB * 1024 * 1024))" --label "PHASE9_RXFS_TEST"

# Copy the old SSP candidate only as a temporary compatibility fallback.
# The original SSP test image is never modified.
cp --reflink=auto "$SSP_SOURCE" "$SSP"

dd if=/dev/zero of="$DISK" bs=1M count="$TOTAL_MB" status=none
sgdisk --zap-all "$DISK" >/dev/null
RXFS_GUID="52584653-5354-4152-4C49-58534F465400"
SSP_GUID="53535000-5354-4152-4C49-58534F465400"
sgdisk -n 1:0:+${ESP_MB}M -t 1:ef00 -c 1:"STARLIX_ESP" "$DISK" >/dev/null
sgdisk -n 2:0:+${RXFS_MB}M -t 2:"$RXFS_GUID" -c 2:"STARLIX_RXFS_TEST" "$DISK" >/dev/null
sgdisk -n 3:0:+${SSP_MB}M -t 3:"$SSP_GUID" -c 3:"STARLIX_SSP_FALLBACK" "$DISK" >/dev/null

first_sector() { sgdisk -i "$1" "$DISK" | awk '/First sector:/ {print $3}'; }
ESP_START="$(first_sector 1)"
RXFS_START="$(first_sector 2)"
SSP_START="$(first_sector 3)"
dd if="$ESP" of="$DISK" bs=512 seek="$ESP_START" conv=notrunc status=none
dd if="$RXFS" of="$DISK" bs=512 seek="$RXFS_START" conv=notrunc status=none
dd if="$SSP" of="$DISK" bs=512 seek="$SSP_START" conv=notrunc status=none

TOTAL_SECTORS=$((TOTAL_MB * 2048))
cat > "$VMDK" <<EOF
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"
RW $TOTAL_SECTORS FLAT "$(basename "$DISK")" 0
#DDB
ddb.adapterType = "sata"
ddb.geometry.cylinders = "$(awk -v s="$TOTAL_SECTORS" 'BEGIN { printf "%d", s/(16*63) }')"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
EOF

cat > "$VMX" <<EOF
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "Starlix OS 1.0 Beta - RXFS Phase 9 Discovery Test"
guestOS = "other-64"
firmware = "efi"
memsize = "1024"
numvcpus = "2"
svga.vramSize = "134217728"
ide0:0.present = "TRUE"
ide0:0.fileName = "$(basename "$VMDK")"
ide0:0.deviceType = "disk"
ide0:0.redo = ""
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "starlix-rxfs-phase9.serial.log"
keyboard.present = "TRUE"
mouse.present = "TRUE"
EOF

sha256sum "$ESP" "$RXFS" "$SSP" "$DISK" > "$RUN/SHA256SUMS.txt"
sgdisk -v "$DISK"
echo
echo "ISOLATED RXFS PHASE 9 CANDIDATE CREATED:"
echo "  VMX : $VMX"
echo "  DISK: $DISK"
echo "  RXFS partition deliberately contains no sys/BaseSystem.dmg."
echo "  Expected diagnostic: RXFS mounted, then BaseSystem open fails, then legacy SSP fallback."
echo "  This tests partition discovery only; it is not a real BaseSystem boot."
echo "  Physical disks and image/starlix.img were not touched."
