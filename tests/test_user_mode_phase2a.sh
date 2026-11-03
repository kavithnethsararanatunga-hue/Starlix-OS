#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -include "$ROOT/tests/freestanding_host_types.hpp" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/user_mode_phase2a_selftest.cpp" \
  "$ROOT/kernel/src/process/process.cpp" \
  -o "$TMP/user_mode_phase2a_selftest"
"$TMP/user_mode_phase2a_selftest"
