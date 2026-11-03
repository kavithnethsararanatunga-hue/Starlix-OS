#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/kernel_stack_phase2g_selftest.cpp" \
  "$ROOT/kernel/src/mem/address_space_core.cpp" \
  "$ROOT/kernel/src/mem/kernel_stack.cpp" \
  -o "$TMP/kernel_stack_phase2g_selftest"
"$TMP/kernel_stack_phase2g_selftest"
