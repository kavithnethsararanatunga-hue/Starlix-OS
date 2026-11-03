#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tools/lbr"

clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/lbrpack.cpp" -o "$DIR/lbrpack" -lcrypto
clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/lbr_tool.cpp" -o "$DIR/lbr-tool" -lcrypto
clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/elf2lbr.cpp" -o "$DIR/elf2lbr"

echo "[✓] Built LBR Toolchain: lbrpack, lbr-tool, elf2lbr"
