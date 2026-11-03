#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE="$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/intlgpudvs.sys"
PROBE="$ROOT/tests/kext_dry_run_host_probe.cpp"
TMP="$(mktemp -d /tmp/starlix-kext-kernel-dynamic-stage.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
[[ -s "$PACKAGE" ]] || { echo "Missing SYS package: $PACKAGE" >&2; exit 2; }
for CXX in clang++ g++; do
    command -v "$CXX" >/dev/null
    echo "[$CXX] KEXT dynamic page-staging preflight (ASan/UBSan)"
    "$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/include" -I"$ROOT/kernel/src" \
        "$PROBE" -o "$TMP/kext-dynamic-stage-$CXX"
    "$TMP/kext-dynamic-stage-$CXX" "$PACKAGE"
done
echo 'KEXT_DYNAMIC_STAGE_HOST_TESTS: PASS (Clang/GCC ASan/UBSan; temporary pages scrubbed/released; no entry execution)'
