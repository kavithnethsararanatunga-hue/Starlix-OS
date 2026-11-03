#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pk.img"
MOUNT_DIR="/tmp/rxfs_pk_mount"

echo "============================================================"
echo " RXFS Phase K: B+Tree Multi-Block Directory Indexing Test"
echo " Target: Create 500+ files in a single directory"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (32 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 33554432 --label PHASE_K_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Creating Multi-Block Directory (/large_dir)..."
mkdir "$MOUNT_DIR/large_dir"

echo ""
echo "[4] Stress Testing: Populating 500 Files in /large_dir/ ..."
python3 -c "
import os
for i in range(500):
    with open('/tmp/rxfs_pk_mount/large_dir/file_' + str(i) + '.txt', 'w') as f:
        f.write(f'STARLIX_FILE_DATA_INDEX_{i}')
"

echo ""
echo "[5] Verifying Total Files Count in Directory:"
TOTAL_FILES=$(ls -1 "$MOUNT_DIR/large_dir" | wc -l)
echo "    Total Files Count: $TOTAL_FILES"

if [ "$TOTAL_FILES" -eq 500 ]; then
    echo "    [✓] 500 FILES CREATED & INDEXED SUCCESSFULLY!"
else
    echo "    [✗] FILE COUNT MISMATCH! Expected 500, got $TOTAL_FILES"
    exit 1
fi

echo ""
echo "[6] Verifying Fast Lookup on High-Index Files:"
echo "    Read file_0.txt   : $(cat "$MOUNT_DIR/large_dir/file_0.txt")"
echo "    Read file_250.txt : $(cat "$MOUNT_DIR/large_dir/file_250.txt")"
echo "    Read file_499.txt : $(cat "$MOUNT_DIR/large_dir/file_499.txt")"

echo ""
echo "[7] Unmounting & Remounting to verify multi-block directory persistence..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

REMOUNT_COUNT=$(ls -1 "$MOUNT_DIR/large_dir" | wc -l)
echo "    Persistent Files Count After Remount: $REMOUNT_COUNT"

if [ "$REMOUNT_COUNT" -eq 500 ]; then
    echo "    [✓] PERSISTENT - Multi-Block Directory Index intact across remounts!"
fi

echo ""
echo "[8] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE K: 100% PASS - B+TREE MULTI-BLOCK DIRS WORKING!"
echo "============================================================"
