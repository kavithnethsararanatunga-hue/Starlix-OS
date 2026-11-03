#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_rw.img"
MOUNT_DIR="/tmp/rxfs_rw_mount"

echo "============================================================"
echo " RXFS v1.0 Phase 3: Full Read-Write (RW) Verification Test"
echo "============================================================"

# Force unmount any stale mounts from previous runs & recreate clean folder
fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume Image..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 1073741824 --label STARLIX_RW_TEST

echo ""
echo "[2] Mounting RXFS Volume in READ-WRITE mode..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Performing POSIX Read-Write Operations:"
echo "------------------------------------------------------------"

echo "$ echo 'Hello from Starlix OS User Space!' > $MOUNT_DIR/user_file.txt"
echo 'Hello from Starlix OS User Space!' > "$MOUNT_DIR/user_file.txt"

echo "$ mkdir $MOUNT_DIR/system_apps"
mkdir "$MOUNT_DIR/system_apps"

echo "$ rm $MOUNT_DIR/welcome.txt"
rm "$MOUNT_DIR/welcome.txt"

echo ""
echo "$ ls -la $MOUNT_DIR"
ls -la "$MOUNT_DIR"

echo ""
echo "$ cat $MOUNT_DIR/user_file.txt"
cat "$MOUNT_DIR/user_file.txt"

echo "------------------------------------------------------------"

echo ""
echo "[4] Unmounting Volume to flush disk writes..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

echo ""
echo "[5] RE-MOUNTING Volume to verify PERSISTENT On-Disk Storage..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "------------------------------------------------------------"
echo "$ ls -la $MOUNT_DIR (After Remount)"
ls -la "$MOUNT_DIR"

echo ""
echo "$ cat $MOUNT_DIR/user_file.txt (After Remount)"
cat "$MOUNT_DIR/user_file.txt"
echo "------------------------------------------------------------"

echo ""
echo "[6] Final Cleanup Unmount..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] RXFS PHASE 3 FULL READ-WRITE TEST: 100% SUCCESS!"
echo "============================================================"
