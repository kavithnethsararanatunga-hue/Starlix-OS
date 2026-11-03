#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
BIN="$ROOT/build/tests/ect_loader_phase2j_selftest"
mkdir -p "$ROOT/build/tests"
FLAGS=( -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer )
"$CXX" "${FLAGS[@]}" \
  "$ROOT/kernel/src/loader/ect_format.cpp" \
  "$ROOT/tests/ect_loader_phase2j_selftest.cpp" \
  -o "$BIN"
"$BIN" "$ROOT/RXFS/sys/User/stlcs.ect"
