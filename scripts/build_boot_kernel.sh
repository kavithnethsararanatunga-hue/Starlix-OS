#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== 1. Build freestanding kernel ==="
if [ "${STARLIX_ENABLE_RING3_SMOKE_TEST:-0}" = "1" ]; then
    echo "[!] Opt-in Ring-3 hardware smoke-test payload is enabled for this build."
fi
./scripts/build_kernel.sh

ENTRY=$(readelf -h kernel/build/kernel.elf | awk '/Entry point/ {print $4}')
echo "Kernel entry: $ENTRY"

echo "=== 2. Build pack_kernel_ect ==="
clang++ -std=c++26 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto -I tools/ect/include tools/ect/pack_kernel_ect.cpp -o tools/ect/pack_kernel_ect

echo "=== 3. Pack RxKernel1.0.ect ==="
./tools/ect/pack_kernel_ect kernel/build/kernel.bin RxKernel1.0.ect "$ENTRY"

echo "=== 4. Build EFI stages ==="
./scripts/build.sh

echo "=== 5. Pack SSP + disk ==="
./scripts/make_ssp.sh
./scripts/make_disk.sh

echo "[✓] Build Complete! Starlix Disk Image Ready."
echo "[✓] QEMU is completely removed."
