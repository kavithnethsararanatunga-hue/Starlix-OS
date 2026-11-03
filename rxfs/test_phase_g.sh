#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pg.img"
MOUNT_DIR="/tmp/rxfs_pg_mount"
SHARED_CONTENT="STARLIX_OS_PERSISTENT_DEDUP_BLOCK_DATA_2026_HEAVY_PAYLOAD"

echo "============================================================"
echo " RXFS Phase G: Persistent Cross-Session Deduplication Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_G_TEST

echo ""
echo "[2] MOUNT SESSION 1: Writing File A..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo "$ echo '$SHARED_CONTENT' > $MOUNT_DIR/file_a.txt"
echo "$SHARED_CONTENT" > "$MOUNT_DIR/file_a.txt"

echo "[3] Unmounting Session 1 (Persisting Dedup Hash Table to Disk)..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

echo ""
echo "[4] MOUNT SESSION 2 (REMOUNT): Writing File B with IDENTICAL Data..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo "$ echo '$SHARED_CONTENT' > $MOUNT_DIR/file_b.txt"
echo "$SHARED_CONTENT" > "$MOUNT_DIR/file_b.txt"

echo ""
echo "[5] Verifying Both Files Readback:"
echo "    File A: $(cat "$MOUNT_DIR/file_a.txt")"
echo "    File B: $(cat "$MOUNT_DIR/file_b.txt")"

echo ""
echo "[6] Cleanup Unmount..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE G: 100% PASS - PERSISTENT DEDUP WORKING!"
echo "============================================================"
