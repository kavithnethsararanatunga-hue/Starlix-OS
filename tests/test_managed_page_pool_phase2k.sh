#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/managed_page_pool_phase2k_selftest.cpp" \
  "$ROOT/kernel/src/mem/managed_page_pool.cpp" \
  -o "$TMP/managed_page_pool_phase2k_selftest"
"$TMP/managed_page_pool_phase2k_selftest"
