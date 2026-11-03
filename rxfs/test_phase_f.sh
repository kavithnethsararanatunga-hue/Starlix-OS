#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pf.img"
MOUNT_DIR="/tmp/rxfs_pf_mount"
HEALTHY_TEXT="STARLIX_OS_HIGH_INTEGRITY_DATA_BLOCK_2026"

echo "============================================================"
echo " RXFS Phase F: Bit-Rot Corruption Protection Test"
echo "============================================================"

# Rebuild driver to ensure latest binaries
"$ROOT/build.sh" > /dev/null 2>&1 || true

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_F_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Data with Automatic SHA-256 Per-Block Cryptographic Hash..."
echo "$HEALTHY_TEXT" > "$MOUNT_DIR/integrity.txt"

echo "    Readback before corruption: $(cat "$MOUNT_DIR/integrity.txt")"

echo ""
echo "[4] Unmounting Volume to Inject Hardware Bit-Rot Corruption..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

echo ""
echo "[5] SIMULATING HARDWARE BIT-ROT: Flipping Byte 50 Inside Compressed Payload..."

DATA_LBA=$(python3 -c "
import subprocess, re
out = subprocess.check_output(['$ROOT/tools/rxfs-inspect', '$IMAGE']).decode()
m = re.search(r'Data Blocks\s+:\s+LBA\s+(\d+)', out)
print(m.group(1) if m else 528)
")

TARGET_LBA=$((DATA_LBA + 3))
TARGET_OFFSET=$((TARGET_LBA * 4096 + 50)) # Target byte 50 inside payload

echo "    Data Base LBA    : $DATA_LBA"
echo "    Target Block LBA : $TARGET_LBA"
echo "    Corrupt Byte Pos : $TARGET_OFFSET"

# Corrupt payload byte
printf '\xFF' | dd of="$IMAGE" bs=1 seek=$TARGET_OFFSET conv=notrunc status=none

echo ""
echo "[6] Remounting Volume to Test Automatic Bit-Rot Detection..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[7] Attempting to Read Corrupted Block (Expecting Protection Block):"
if cat "$MOUNT_DIR/integrity.txt" 2>/dev/null; then
    echo "    [✗] PROTECTION FAILED! Corrupted data was returned to user space!"
    exit 1
else
    echo "    [✓] PROTECTION SUCCESSFUL! Driver detected Bit-Rot Hash Mismatch & blocked read!"
fi

echo ""
echo "[8] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE F: 100% PASS - BIT-ROT PROTECTION WORKING!"
echo "============================================================"
