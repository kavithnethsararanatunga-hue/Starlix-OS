#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[*] Installing OpenSSL, Zstd & FUSE3 Development Headers..."
sudo apt install -y libfuse3-dev fuse3 libzstd-dev libssl-dev > /dev/null 2>&1 || true

echo "[*] Building RXFS Phase L Tools & Drivers (C++26)..."
clang++ -std=c++26 -O2 -I"$ROOT/include" "$ROOT/tools/mkfs_rxfs.cpp" -o "$ROOT/tools/mkfs.rxfs"
clang++ -std=c++26 -O2 -I"$ROOT/include" "$ROOT/tools/rxfs_inspect.cpp" -o "$ROOT/tools/rxfs-inspect"
clang++ -std=c++26 -O2 -I"$ROOT/include" "$ROOT/tools/rxfs_snapshot.cpp" -o "$ROOT/tools/rxfs-snapshot"
clang++ -std=c++26 -O2 -I"$ROOT/include" "$ROOT/tools/rxfs_attr.cpp" -o "$ROOT/tools/rxfs-attr"
clang++ -std=c++26 -O2 -DFUSE_USE_VERSION=31 -I"$ROOT/include" "$ROOT/tools/mount_rxfs.cpp" -o "$ROOT/tools/mount.rxfs" -lfuse3 -lzstd -lcrypto

echo "[✓] Built Successfully:"
echo "    - rxfs/tools/mkfs.rxfs"
echo "    - rxfs/tools/rxfs-inspect"
echo "    - rxfs/tools/rxfs-snapshot"
echo "    - rxfs/tools/rxfs-attr (Attribute CLI Manager)"
echo "    - rxfs/tools/mount.rxfs (Full Feature FUSE Driver)"
