#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
SYS="$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/intlgpudvs.sys"
TMP="$(mktemp -d /tmp/starlix-kext-runtime.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -f "$SYS" ]]; then
    bash "$ROOT/scripts/build_intlgpudvs.sh"
fi

"$CXX" \
    -std=c++20 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D_GNU_SOURCE -I"$ROOT/kernel/src" -I"$ROOT/include" \
    "$ROOT/tests/kext_runtime_host_probe.cpp" \
    -o "$TMP/kext_runtime_host_probe"

"$TMP/kext_runtime_host_probe" "$SYS"
