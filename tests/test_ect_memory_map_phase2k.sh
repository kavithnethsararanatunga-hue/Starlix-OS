#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
INPUT="$ROOT/RXFS/sys/User/stlcs.ect"
[[ -f "$INPUT" ]] || { echo "missing ECT test input: $INPUT" >&2; exit 1; }
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/ect_memory_map_phase2k_selftest.cpp" \
  "$ROOT/kernel/src/loader/ect_memory_map.cpp" \
  "$ROOT/kernel/src/loader/ect_format.cpp" \
  -o "$TMP/ect_memory_map_phase2k_selftest"
"$TMP/ect_memory_map_phase2k_selftest" "$INPUT"
