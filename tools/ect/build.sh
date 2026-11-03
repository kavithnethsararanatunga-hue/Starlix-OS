#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="$ROOT/tools/ect"

clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/ectpack.cpp" -o "$DIR/ectpack"
clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/ect_tool.cpp" -o "$DIR/ect-tool"
clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/elf2ect.cpp" -o "$DIR/elf2ect"
clang++ -std=c++26 -O2 -I"$DIR/include" "$DIR/starlix_runtime.cpp" -o "$DIR/starlix-runtime"

echo "[✓] Built ECT Toolchain: ectpack, ect-tool, elf2ect, starlix-runtime"
