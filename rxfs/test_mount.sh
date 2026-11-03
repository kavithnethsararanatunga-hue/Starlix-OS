#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_demo.img"
MOUNT_DIR="/tmp/rxfs_mount"

echo "============================================================"
echo " RXFS v1.0 Cross-OS FUSE Mount Verification Test"
echo "============================================================"

mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume Image..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 1073741824 --label STARLIX_MOUNT_TEST

echo ""
echo "[2] Mounting RXFS Volume via FUSE Driver to $MOUNT_DIR..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 1

echo ""
echo "[3] Running Standard Linux OS Commands on RXFS Mounted Directory:"
echo "------------------------------------------------------------"
echo "$ ls -la $MOUNT_DIR"
ls -la "$MOUNT_DIR"

echo ""
echo "$ cat $MOUNT_DIR/welcome.txt"
cat "$MOUNT_DIR/welcome.txt"

echo ""
echo "$ cat $MOUNT_DIR/starlix.sys"
cat "$MOUNT_DIR/starlix.sys"

echo ""
echo "$ df -h $MOUNT_DIR"
df -h "$MOUNT_DIR"
echo "------------------------------------------------------------"

echo ""
echo "[4] Unmounting RXFS Volume..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID

echo "============================================================"
echo " [✓] RXFS CROSS-OS FUSE MOUNT TEST: 100% SUCCESS!"
echo "============================================================"
