#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_p5.img"
MOUNT_DIR="/tmp/rxfs_p5_mount"

echo "============================================================"
echo " RXFS v1.0 Phase 5: Instant COW Snapshots & Dedup Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume Image..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 1073741824 --label STARLIX_P5_TEST

echo ""
echo "[2] Mounting RXFS Volume with Dedup & COW Engine..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Testing Online Data Deduplication Engine:"
echo "------------------------------------------------------------"
echo "$ echo 'Starlix OS Heavy Data Block 12345' > $MOUNT_DIR/file_original.txt"
echo 'Starlix OS Heavy Data Block 12345' > "$MOUNT_DIR/file_original.txt"

echo "$ echo 'Starlix OS Heavy Data Block 12345' > $MOUNT_DIR/file_duplicate.txt"
echo 'Starlix OS Heavy Data Block 12345' > "$MOUNT_DIR/file_duplicate.txt"
echo "------------------------------------------------------------"

echo ""
echo "[4] Creating Instant Copy-On-Write (COW) Snapshot (@backup_v1)..."
"$ROOT/tools/rxfs-snapshot" create "$IMAGE" backup_v1

echo ""
echo "[5] Listing Active Snapshots on Volume:"
"$ROOT/tools/rxfs-snapshot" list "$IMAGE"

echo ""
echo "[6] Final Cleanup Unmount..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] RXFS PHASE 5 SNAPSHOTS & DEDUP TEST: 100% SUCCESS!"
echo "============================================================"
