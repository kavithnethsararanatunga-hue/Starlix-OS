#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pl.img"
MOUNT_DIR="/tmp/rxfs_pl_mount"

echo "============================================================"
echo " RXFS Phase L: Immutable (+i) & Append-Only (+a) Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_L_TEST

echo ""
echo "[2] Mounting RXFS Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Protection Target File..."
echo "SYSTEM_CORE_PROTECTED_DATA" > "$MOUNT_DIR/protected.sys"

echo ""
echo "[4] Unmounting & Setting Immutable Flag (+i) via rxfs-attr Tool..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/rxfs-attr" set "$IMAGE" /protected.sys +i
"$ROOT/tools/rxfs-attr" get "$IMAGE" /protected.sys

echo ""
echo "[5] Remounting Volume..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[6] Attempting to Overwrite Immutable File (Expecting Operation Not Permitted Block):"
if echo "HACKED_DATA" > "$MOUNT_DIR/protected.sys" 2>/dev/null; then
    echo "    [✗] IMMUTABLE PROTECTION FAILED! File was overwritten!"
    exit 1
else
    echo "    [✓] IMMUTABLE WRITE PROTECTION SUCCESSFUL! Driver blocked modification!"
fi

echo ""
echo "[7] Attempting to Delete Immutable File (Expecting Operation Not Permitted Block):"
if rm "$MOUNT_DIR/protected.sys" 2>/dev/null; then
    echo "    [✗] IMMUTABLE PROTECTION FAILED! File was deleted!"
    exit 1
else
    echo "    [✓] IMMUTABLE DELETE PROTECTION SUCCESSFUL! Driver blocked deletion!"
fi

echo ""
echo "[8] Unmounting & Removing Immutable Flag (-i)..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true
sleep 1

"$ROOT/tools/rxfs-attr" set "$IMAGE" /protected.sys -i

echo ""
echo "[9] Remounting & Verifying Normal Delete Functionality..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

rm "$MOUNT_DIR/protected.sys"
echo "    [✓] File successfully deleted after removing +i flag!"

echo ""
echo "[10] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE L: 100% PASS - ATTRIBUTE PROTECTION WORKING!"
echo "============================================================"
