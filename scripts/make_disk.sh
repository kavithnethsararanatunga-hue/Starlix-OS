#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$ROOT/image/starlix.img"
VMDK="$ROOT/image/starlix.vmdk"
ESP_IMG="$ROOT/image/esp.img"
SSP_IMG="$ROOT/image/ssp.img"

ESP_SIZE_MB=64
SSP_SIZE_MB=128
TOTAL_SIZE_MB=$((ESP_SIZE_MB + SSP_SIZE_MB + 2))

SSP_TYPE_GUID="53535000-5354-4152-4C49-58534F465400"

echo "[*] Building ESP (FAT32) image ..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=$ESP_SIZE_MB status=none
mkfs.vfat -F32 "$ESP_IMG" > /dev/null

mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mmd -i "$ESP_IMG" ::/EFI/OpenRPX
mmd -i "$ESP_IMG" ::/EFI/OpenRPX/xsOft
mmd -i "$ESP_IMG" ::/EFI/OpenRPX/xsOft/Boot

mcopy -i "$ESP_IMG" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi" ::/EFI/OpenRPX/xsOft/Boot/stbtmngr.efi

if [ ! -f "$SSP_IMG" ]; then
    echo "[!] SSP image not found."
    exit 1
fi

echo "[*] Creating GPT disk image ($TOTAL_SIZE_MB MB) ..."
truncate -s ${TOTAL_SIZE_MB}M "$IMG"

sgdisk --zap-all "$IMG" > /dev/null
sgdisk -n 1:0:+${ESP_SIZE_MB}M -t 1:ef00 -c 1:"STARLIX_ESP" "$IMG" > /dev/null
sgdisk -n 2:0:+${SSP_SIZE_MB}M -t 2:${SSP_TYPE_GUID} -c 2:"STARLIX_SSP" "$IMG" > /dev/null

get_first_sector() {
    sgdisk -i "$1" "$IMG" | awk '/First sector:/ {print $3}'
}

ESP_START_SECTOR=$(get_first_sector 1)
SSP_START_SECTOR=$(get_first_sector 2)

dd if="$ESP_IMG" of="$IMG" bs=512 seek=$ESP_START_SECTOR conv=notrunc status=none
dd if="$SSP_IMG" of="$IMG" bs=512 seek=$SSP_START_SECTOR conv=notrunc status=none

# Auto-generate starlix.vmdk descriptor for VirtualBox / VMware
cat > "$VMDK" << VMDK_EOF
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW 397312 FLAT "starlix.img" 0

# The Disk Data Base
#DDB
ddb.adapterType = "sata"
ddb.geometry.cylinders = "394"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
VMDK_EOF

echo "[✓] Disk Image Created: $IMG"
echo "[✓] VMDK Descriptor Created: $VMDK"
