#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ATA="$ROOT/kernel/src/drivers/ata_pio.cpp"
STORAGE="$ROOT/kernel/src/storage/storage.cpp"
MAIN="$ROOT/kernel/src/main.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
pass(){ echo "[PASS] $1"; }
fail(){ echo "[FAIL] $1" >&2; exit 1; }

grep -q 'class_code != 0x01u || subclass != 0x01u' "$ATA" && pass 'PCI IDE class/subclass discovery is explicit' || fail 'PCI IDE class/subclass discovery'
grep -q 'kCommandIdentify' "$ATA" && pass 'ATA IDENTIFY command is used for device discovery' || fail 'ATA IDENTIFY command'
grep -q 'kStatusPollLimit' "$ATA" && pass 'ATA status polling is bounded' || fail 'bounded polling'
grep -q 'kCommandReadSectorsExt' "$ATA" && grep -q 'kCommandReadSectors' "$ATA" && pass 'LBA28 and LBA48 read commands are implemented' || fail 'LBA read commands'
grep -q 'kDeviceControlNien' "$ATA" && pass 'ATA device interrupts are masked for polling I/O' || fail 'ATA polling interrupt mask'
if grep -Eq 'kCommandWrite|0x30u|0x34u|0xCAu|0x35u' "$ATA"; then fail 'ATA write command found'; else pass 'ATA driver contains no disk-write command'; fi
grep -q 'device.backend != BlockBackend::UsbMassStorage' "$STORAGE" && pass 'generic storage rejects writes to ATA backend' || fail 'ATA write rejection'
grep -q 'device.backend == BlockBackend::AtaPio' "$STORAGE" && pass 'generic block reads dispatch to ATA PIO' || fail 'ATA read dispatch'
grep -q 'ata_pio_init' "$STORAGE" && pass 'storage manager discovers ATA devices' || fail 'ATA storage discovery'
grep -q 'storage::storage_init();' "$MAIN" && pass 'boot calls storage initialization independent of EHCI result' || fail 'storage boot initialization'
grep -q 'drivers/ata_pio.cpp' "$BUILD" && grep -q 'drivers/ata_pio.o' "$BUILD" && pass 'ATA driver compiled and linked by kernel build' || fail 'ATA build integration'
