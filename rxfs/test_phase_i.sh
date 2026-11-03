#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="/tmp/rxfs_pi.img"
MOUNT_DIR="/tmp/rxfs_pi_mount"
CRASH_DATA="STARLIX_OS_JOURNALED_TRANSACTION_RECOVERY_2026"

echo "============================================================"
echo " RXFS Phase I: Crash-Safe Write-Ahead Journaling Test"
echo "============================================================"

fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true
rm -rf "$MOUNT_DIR" "$IMAGE"
mkdir -p "$MOUNT_DIR"

echo "[1] Formatting RXFS Volume (16 MB)..."
"$ROOT/tools/mkfs.rxfs" "$IMAGE" --size 16777216 --label PHASE_I_TEST

echo ""
echo "[2] Mounting RXFS Volume with WAL Journaling Driver..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[3] Writing Data File (Journal Logged to WAL Area LBA 100)..."
echo "$CRASH_DATA" > "$MOUNT_DIR/wal_test.txt"

echo ""
echo "[4] SIMULATING SUDDEN SYSTEM CRASH / POWER LOSS (kill -9 FUSE Driver without Unmount)..."
kill -9 $FUSE_PID 2>/dev/null || true
sleep 1
fusermount3 -u "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true

echo ""
echo "[5] Remounting Volume (Triggers Automatic Journal Scan & Crash Recovery Replay)..."
"$ROOT/tools/mount.rxfs" "$IMAGE" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

echo ""
echo "[6] VERIFYING RECOVERED FILE CONTENT AFTER CRASH:"
echo "------------------------------------------------------------"
RECOVERED_TEXT=$(cat "$MOUNT_DIR/wal_test.txt")
echo "  Recovered Content : $RECOVERED_TEXT"
echo "------------------------------------------------------------"

if [ "$RECOVERED_TEXT" = "$CRASH_DATA" ]; then
    echo "  [✓] CRASH-SAFE JOURNAL RECOVERY PASSED! Zero Data Lost!"
else
    echo "  [✗] JOURNAL RECOVERY FAILED!"
    exit 1
fi

echo ""
echo "[7] Cleanup..."
fusermount3 -u "$MOUNT_DIR" || sudo umount "$MOUNT_DIR" || kill -9 $FUSE_PID 2>/dev/null || true

echo "============================================================"
echo " [✓] PHASE I: 100% PASS - CRASH-SAFE JOURNALING WORKING!"
echo "============================================================"
