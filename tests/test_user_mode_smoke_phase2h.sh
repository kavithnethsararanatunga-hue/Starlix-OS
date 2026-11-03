#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/user_mode_smoke_phase2h_selftest.cpp" \
  -o "$TMP/user_mode_smoke_phase2h_selftest"
"$TMP/user_mode_smoke_phase2h_selftest"
