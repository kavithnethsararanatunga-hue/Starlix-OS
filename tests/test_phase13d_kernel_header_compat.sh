#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE="$ROOT/tests/kernel_primitive_header_probe.cpp"
TMP="$(mktemp -d /tmp/starlix-kext-kernel-headers.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
for CXX in clang++ g++; do
    command -v "$CXX" >/dev/null || { echo "Missing compiler: $CXX" >&2; exit 2; }
    echo "[$CXX] kernel primitive/header compatibility (freestanding)"
    # Add only the repository root so the probe can include kernel/src/...;
    # deliberately do not add -Iinclude. The SYS headers must resolve their
    # own siblings and must not import libc typedefs into the kernel TU.
    "$CXX" -std=c++26 -ffreestanding -fno-exceptions -fno-rtti \
        -Wall -Wextra -Werror -I"$ROOT" -c "$PROBE" -o "$TMP/kernel-headers-$CXX.o"
done
echo 'PHASE13D_KERNEL_HEADER_COMPAT: PASS (Clang/GCC; no libc typedef collision; no -Iinclude dependency)'
