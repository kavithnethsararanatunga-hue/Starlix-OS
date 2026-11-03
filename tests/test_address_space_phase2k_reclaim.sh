#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/address_space_phase2k_reclaim_selftest.cpp" \
  "$ROOT/kernel/src/mem/address_space_core.cpp" \
  -o "$TMP/address_space_phase2k_reclaim_selftest"
"$TMP/address_space_phase2k_reclaim_selftest"
