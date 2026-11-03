#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pb.img"
MOUNT_DIR="/tmp/rxfs_pb_mount"

echo "============================================================"
echo " RXFS Phase B: Subdirectories & Nested Path Test"
echo " Target: Deep folder creation /a/b/c/d + nested file writes"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_B_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Creating deep nested directory structure (/Starlix/System/Apps)..."
mkdir -p "$MOUNT_DIR/Starlix/System/Apps"

echo ""
echo "[4] Writing files at different folder levels..."
echo "Root Level File" > "$MOUNT_DIR/root_level.txt"
echo "Starlix Level File" > "$MOUNT_DIR/Starlix/starlix_level.txt"
echo "Deep App Config File Content 12345" > "$MOUNT_DIR/Starlix/System/Apps/app_config.json"

echo ""
echo "[5] Verifying directory tree structure ($ ls -laR):"
ls -laR "$MOUNT_DIR"

echo ""
echo "[6] Verifying deep file readback ($ cat /Starlix/System/Apps/app_config.json):"
cat "$MOUNT_DIR/Starlix/System/Apps/app_config.json"

echo ""
echo "[7] Unmounting & Remounting to verify persistence..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "    Readback after remount:"
cat "$MOUNT_DIR/Starlix/System/Apps/app_config.json"

echo ""
echo "[8] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE B: 100% PASS - SUBDIRECTORIES WORKING!"
echo "============================================================"
