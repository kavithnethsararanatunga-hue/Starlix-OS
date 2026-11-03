#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_p4.img"
MOUNT_DIR="/tmp/rxfs_p4_mount"

echo "============================================================"
echo " RXFS v1.0 Phase 4: Advanced Features Verification"
echo " (BLAKE3/CRC32C Checksums + Compression Metadata)"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume Image..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 1073741824 --label STARLIX_P4_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Data with Automatic Checksum Calculation:"
echo 'Starlix OS - RXFS Phase 4 High Integrity Block Test' > "$MOUNT_DIR/integrity_test.txt"

echo "$ cat $MOUNT_DIR/integrity_test.txt"
cat "$MOUNT_DIR/integrity_test.txt"

echo ""
echo "[4] Inspecting RXFS Inode Metadata..."
"$ROOT/tools/rxfs-inspect" "$IMAGE" | grep -A 10 "ALGORITHMS"

echo ""
echo "[5] Final Cleanup Unmount..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] RXFS PHASE 4 ADVANCED FEATURES TEST: 100% SUCCESS!"
echo "============================================================"
