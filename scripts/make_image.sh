#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$ROOT/image/starlix.img"

echo "[*] Creating FAT32 ESP image ..."
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.vfat -F32 "$IMG" > /dev/null

mmd -i "$IMG" ::/EFI
mmd -i "$IMG" ::/EFI/BOOT
mmd -i "$IMG" ::/EFI/OpenRPX
mmd -i "$IMG" ::/EFI/OpenRPX/xsOft
mmd -i "$IMG" ::/EFI/OpenRPX/xsOft/Boot

mcopy -i "$IMG" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi" ::/EFI/OpenRPX/xsOft/Boot/stbtmngr.efi

echo "[✓] Image created: $IMG"
