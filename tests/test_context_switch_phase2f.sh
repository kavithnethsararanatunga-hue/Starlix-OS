#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/context_switch_phase2f_selftest.cpp" \
  -o "$TMP/context_switch_phase2f_selftest"
"$TMP/context_switch_phase2f_selftest"
