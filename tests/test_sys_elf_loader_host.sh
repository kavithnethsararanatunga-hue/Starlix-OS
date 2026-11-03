#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/intlgpudvs.sys"
PROBE="$ROOT/tests/sys_elf_loader_host_probe.cpp"
TMP="$(mktemp -d /tmp/starlix-sys-elf-loader.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

[[ -s "$PACKAGE" ]] || {
    echo "Missing Intel KEXT package: $PACKAGE" >&2
    echo "Run bash scripts/build_intlgpudvs.sh first." >&2
    exit 2
}

for CXX in clang++ g++; do
    command -v "$CXX" >/dev/null || { echo "Missing compiler: $CXX" >&2; exit 2; }
    echo "[$CXX] STSYS001 + ELF64 section layout + relocation host probe (ASan/UBSan)"
    "$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/include" "$PROBE" -o "$TMP/sys-elf-loader-$CXX"
    "$TMP/sys-elf-loader-$CXX" "$PACKAGE"
done

echo "SYS_ELF_LOADER_HOST_TESTS: PASS (Clang/GCC ASan/UBSan; relocation only, no driver execution)"
