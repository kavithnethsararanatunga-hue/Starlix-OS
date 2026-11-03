#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_ph.img"
MOUNT_DIR="/tmp/rxfs_ph_mount"

echo "============================================================"
echo " RXFS Phase H: True Copy-on-Write (COW) & /.snapshots/ Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_H_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Original File Version 1..."
echo "ORIGINAL_VERSION_1_DATA" > "$MOUNT_DIR/document.txt"
echo "    Original File Content: $(cat "$MOUNT_DIR/document.txt")"

echo ""
echo "[4] Unmounting & Taking Instant COW Snapshot (@snap_v1)..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/rxfs-snapshot" create "$IMAGE" snap_v1

echo ""
echo "[5] Remounting Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[6] Modifying Active File Version (Triggers Copy-On-Write Data Allocation)..."
echo "MODIFIED_VERSION_2_DATA" > "$MOUNT_DIR/document.txt"

echo ""
echo "[7] VERIFYING TRUE COW SNAPSHOT ISOLATION:"
echo "------------------------------------------------------------"
ACTIVE_VAL=$(cat "$MOUNT_DIR/document.txt")
SNAP_VAL=$(cat "$MOUNT_DIR/.snapshots/snap_v1/document.txt")

echo "  Active File Path   : /document.txt                  -> $ACTIVE_VAL"
echo "  Snapshot File Path : /.snapshots/snap_v1/document.txt -> $SNAP_VAL"
echo "------------------------------------------------------------"

if [ "$ACTIVE_VAL" = "MODIFIED_VERSION_2_DATA" ] && [ "$SNAP_VAL" = "ORIGINAL_VERSION_1_DATA" ]; then
    echo "  [✓] TRUE COPY-ON-WRITE (COW) SNAPSHOT ISOLATION PASSED!"
else
    echo "  [✗] COW ISOLATION FAILED!"
    exit 1
fi

echo ""
echo "[8] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE H: 100% PASS - TRUE COW SNAPSHOTS WORKING!"
echo "============================================================"
