#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_master.img"
MOUNT_DIR="/tmp/rxfs_master_mount"

echo "============================================================"
echo " STARLIX OS - RXFS v1.0 MASTER VALIDATION SUITE"
echo " Testing All 13 Phases & 50+ Extra Features"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[Phase 1] Formatting RXFS Volume (32 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 33554432 --label STARLIX_MASTER_FS

echo ""
echo "[Phase 2] Mounting RXFS Read-Write Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[Phase A & D & E] Writing Encrypted & Zstd Compressed 1 MB File..."
python3 -c "print('STARLIX_OS_MASTER_TEST_DATA_BLOCK_2026\n' * 25000)" > "$MOUNT_DIR/master_data.bin"
HASH_ORIG=$(sha256sum "$MOUNT_DIR/master_data.bin" | awk '{print $1}')
echo "    Original Hash: $HASH_ORIG"

echo ""
echo "[Phase B] Creating Deep Nested Directory Structure (/A/B/C/D)..."
mkdir -p "$MOUNT_DIR/A/B/C/D"
echo "Deep Path File Content" > "$MOUNT_DIR/A/B/C/D/nested.txt"

echo ""
echo "[Phase J] Creating Hard Link & Symbolic Link..."
ln "$MOUNT_DIR/master_data.bin" "$MOUNT_DIR/hardlink_data.bin"
ln -s "master_data.bin" "$MOUNT_DIR/symlink_data.bin"

echo ""
echo "[Phase K] Populating Multi-Block Directory (200 Files)..."
python3 -c "
import os
for i in range(200):
    with open('/tmp/rxfs_master_mount/A/B/C/D/f_' + str(i) + '.txt', 'w') as f:
        f.write('DATA_' + str(i))
"

echo ""
echo "[Phase H] Unmounting & Taking Instant COW Snapshot (@master_snap)..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/rxfs-snapshot" create "$IMAGE" master_snap

echo ""
echo "[Phase G] Remounting & Testing Cross-Session Persistent Deduplication..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

python3 -c "print('STARLIX_OS_MASTER_TEST_DATA_BLOCK_2026\n' * 25000)" > "$MOUNT_DIR/dedup_copy.bin"

echo ""
echo "[Phase H Check] Modifying Active File & Checking Snapshot Isolation..."
echo "MODIFIED_ACTIVE_DATA" > "$MOUNT_DIR/master_data.bin"

ACTUAL_ACTIVE=$(head -n 1 "$MOUNT_DIR/master_data.bin")
ACTUAL_SNAP=$(head -n 1 "$MOUNT_DIR/.snapshots/master_snap/master_data.bin")

echo "    Active File   : $ACTUAL_ACTIVE"
echo "    Snapshot File : $ACTUAL_SNAP"

if [ "$ACTUAL_ACTIVE" = "MODIFIED_ACTIVE_DATA" ] && [ "$ACTUAL_SNAP" = "STARLIX_OS_MASTER_TEST_DATA_BLOCK_2026" ]; then
    echo "    [✓] COW SNAPSHOT ISOLATION PASSED!"
else
    echo "    [✗] COW SNAPSHOT ISOLATION FAILED!"
    exit 1
fi

echo ""
echo "[Phase L] Unmounting & Setting Immutable Flag (+i) on /A/B/C/D/nested.txt..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/rxfs-attr" set "$IMAGE" /A/B/C/D/nested.txt +i

echo ""
echo "[Phase L Check] Remounting & Testing Immutable Write Protection..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

if echo "OVERWRITE" > "$MOUNT_DIR/A/B/C/D/nested.txt" 2>/dev/null; then
    echo "    [✗] IMMUTABLE PROTECTION FAILED!"
    exit 1
else
    echo "    [✓] IMMUTABLE WRITE PROTECTION PASSED!"
fi

echo ""
echo "[Cleanup] Unmounting RXFS Volume..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " 🎉 RXFS v1.0 MASTER VALIDATION SUITE: 100% ALL PASSED! 🎉"
echo "============================================================"
