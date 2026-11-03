#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/kernel/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp"
PROBE="$ROOT/tests/intlgpudvs_phase2_probe.cpp"
TMP="$(mktemp -d /tmp/starlix-intel-gpu-phase2.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

"${CXX:-g++}" -std=c++20 -O0 -g3 -fno-inline -fno-lto \
    -Wall -Wextra -Werror \
    -I"$ROOT/include" \
    "$SRC" "$PROBE" \
    -o "$TMP/intlgpudvs_phase2_probe"

"$TMP/intlgpudvs_phase2_probe"
