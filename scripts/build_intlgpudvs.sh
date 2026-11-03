#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="$ROOT/kernel"
OUT="$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel"
SRC="$KDIR/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp"
BUILD="$OUT/build"
BUNDLE="$OUT/intlgpudvs.kext"
CONTENTS="$BUNDLE/Contents"
STARLIX="$CONTENTS/Starlix"

mkdir -p "$BUILD" "$STARLIX"

for cmd in clang++ g++ nm readelf; do
    command -v "$cmd" >/dev/null || {
        echo "Missing required tool: $cmd" >&2
        exit 1
    }
done

echo "[1/5] Compiling external Intel HD 2500 KEXT from kernel/src/drivers/Gpu/Basic/Intel..."
clang++ \
    -target x86_64-unknown-none \
    -ffreestanding -fno-stack-protector -fno-exceptions -fno-rtti \
    -mno-red-zone -mcmodel=large \
    -std=c++26 -O0 -g3 -fno-inline \
    -fno-omit-frame-pointer -fno-lto \
    -Wall -Wextra -Werror \
    -I"$ROOT/include" \
    -c "$SRC" -o "$BUILD/intlgpudvs.o"

echo "[2/5] Verifying exported driver symbols..."
nm -g --defined-only "$BUILD/intlgpudvs.o" |
    grep -E '[[:space:]]StarlixGpuDriverEntry$'

nm -g --defined-only "$BUILD/intlgpudvs.o" |
    grep -E '[[:space:]]StarlixIntelGpuMatchesPciId$'

nm -g --defined-only "$BUILD/intlgpudvs.o" |
    grep -E '[[:space:]]StarlixGpuFrameDriverEntry$'

nm -g --defined-only "$BUILD/intlgpudvs.o" |
    grep -E '[[:space:]]StarlixGpuKextEntry$'

echo "[3/5] Building SYS packager..."
g++ -std=c++20 -O0 -g3 -fno-inline -fno-lto \
    -Wall -Wextra -Werror \
    -I"$ROOT/include" \
    "$ROOT/tools/sys/pack_sys.cpp" \
    -o "$BUILD/pack_sys"

echo "[4/5] Packaging ELF64 object as STSYS001..."
"$BUILD/pack_sys" \
    "$BUILD/intlgpudvs.o" \
    "$OUT/intlgpudvs.sys" \
    intlgpudvs \
    StarlixGpuKextEntry

cp "$OUT/intlgpudvs.sys" "$STARLIX/intlgpudvs.sys"

echo "[5/5] Creating Starlix KEXT metadata..."
cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>com.starlix.driver.intlgpudvs</string>

    <key>CFBundleName</key>
    <string>intlgpudvs</string>

    <key>CFBundleVersion</key>
    <string>0.2.0</string>

    <key>StarlixExecutable</key>
    <string>intlgpudvs.sys</string>

    <key>StarlixPCIVendorID</key>
    <string>0x8086</string>

    <key>StarlixPCIDeviceID</key>
    <string>0x0152</string>

    <key>StarlixGPUFamily</key>
    <string>IvyBridgeGen7</string>

    <key>StarlixGraphicsBackend</key>
    <string>GOPFramebuffer</string>

    <key>StarlixGraphicsAbiMajor</key>
    <string>2</string>

    <key>StarlixGraphicsAbiMinor</key>
    <string>0</string>

    <key>StarlixDriverAbiMajor</key>
    <string>1</string>

    <key>StarlixDriverAbiMinor</key>
    <string>0</string>

    <key>StarlixKextEntry</key>
    <string>StarlixGpuKextEntry</string>

    <key>StarlixNativeAcceleration</key>
    <string>false</string>
</dict>
</plist>
PLIST

echo
echo "===== INTEL GPU DRIVER OUTPUTS ====="
ls -lh "$BUILD/intlgpudvs.o" \
       "$OUT/intlgpudvs.sys" \
       "$CONTENTS/Info.plist" \
       "$STARLIX/intlgpudvs.sys"

echo
readelf -h "$BUILD/intlgpudvs.o" | grep -E 'Type:|Machine:'
readelf -r "$BUILD/intlgpudvs.o" > "$BUILD/intlgpudvs.relocations.txt"

echo
echo "===== KEXT CONTENTS ====="
find "$BUNDLE" -type f -printf '%P (%s bytes)\n' | sort

echo
sha256sum "$OUT/intlgpudvs.sys" "$STARLIX/intlgpudvs.sys"

echo
echo "PASS: intlgpudvs.sys and intlgpudvs.kext built."
echo "NOTE: source is external to kernel.elf; GOP framebuffer backend only; native Intel acceleration is disabled."
