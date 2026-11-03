#!/bin/bash
# Build the external Starlix basic framebuffer graphics module.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/Rx/Drivers/Gpu/BasicDriver/Nvidia/nvdgpudrs.cpp"
OUT="$ROOT/Rx/Drivers/Gpu/BasicDriver/Nvidia"
BUILD="$OUT/build"
TOOLS="$ROOT/tools/sys"

mkdir -p "$BUILD" "$TOOLS"

echo "[1/4] Compile external GPU module with full debug information..."
clang++ \
  -target x86_64-unknown-none \
  -ffreestanding -fno-stack-protector -fno-exceptions -fno-rtti \
  -mno-red-zone -mcmodel=large \
  -std=c++26 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto \
  -Wall -Wextra -I"$ROOT/include" \
  -c "$SRC" -o "$BUILD/nvdgpudrs.o"

echo "[2/4] Verify ELF module entry symbol..."
nm -g --defined-only "$BUILD/nvdgpudrs.o" |
  grep -E '[[:space:]]StarlixGpuDriverEntry$'

echo "[3/4] Build host-side SYS package tool..."
g++ -std=c++20 -O0 -g3 -fno-inline -fno-lto -Wall -Wextra \
  -I"$ROOT/include" "$ROOT/tools/sys/pack_sys.cpp" \
  -o "$BUILD/pack_sys"

echo "[4/4] Package relocatable ELF object as nvdgpudrs.sys..."
"$BUILD/pack_sys" \
  "$BUILD/nvdgpudrs.o" \
  "$OUT/nvdgpudrs.sys" \
  nvdgpudrs \
  StarlixGpuDriverEntry

echo
echo "===== GPU MODULE OUTPUTS ====="
ls -lh "$BUILD/nvdgpudrs.o" "$OUT/nvdgpudrs.sys"
readelf -h "$BUILD/nvdgpudrs.o" | grep -E 'Type:|Machine:'
echo
echo "Relocations are preserved inside the ELF payload:"
readelf -r "$BUILD/nvdgpudrs.o" > "$BUILD/nvdgpudrs.relocations.txt"
sed -n '1,50p' "$BUILD/nvdgpudrs.relocations.txt"
sha256sum "$OUT/nvdgpudrs.sys"
