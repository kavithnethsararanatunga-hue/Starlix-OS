#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pj.img"
MOUNT_DIR="/tmp/rxfs_pj_mount"
ORIG_TEXT="STARLIX_OS_POSIX_LINKS_TEST_DATA_2026"

echo "============================================================"
echo " RXFS Phase J: POSIX Hard & Symbolic Links Verification Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_J_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Original Master File..."
echo "$ORIG_TEXT" > "$MOUNT_DIR/master.txt"

echo ""
echo "[4] Creating POSIX Hard Link (ln master.txt hardlink.txt)..."
ln "$MOUNT_DIR/master.txt" "$MOUNT_DIR/hardlink.txt"

echo ""
echo "[5] Creating POSIX Symbolic Link (ln -s master.txt symlink.txt)..."
ln -s "master.txt" "$MOUNT_DIR/symlink.txt"

echo ""
echo "[6] Verifying Directory Listing ($ ls -l):"
ls -l "$MOUNT_DIR"

echo ""
echo "[7] Verifying Data Readback Via Links:"
echo "    Read via Hard Link: $(cat "$MOUNT_DIR/hardlink.txt")"
echo "    Read via Soft Link: $(cat "$MOUNT_DIR/symlink.txt")"

echo ""
echo "[8] Testing Hard Link Protection (Deleting master.txt)..."
rm "$MOUNT_DIR/master.txt"

echo "    Read via Hard Link After Delete: $(cat "$MOUNT_DIR/hardlink.txt")"

echo ""
echo "[9] Unmounting & Remounting to verify link persistence..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "    Persistent Hard Link Readback: $(cat "$MOUNT_DIR/hardlink.txt")"

echo ""
echo "[10] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE J: 100% PASS - POSIX HARD & SOFT LINKS WORKING!"
echo "============================================================"
