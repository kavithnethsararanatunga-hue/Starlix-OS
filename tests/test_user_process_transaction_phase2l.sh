#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-clang++}"
OUT="$ROOT/build/tests/user_process_transaction_phase2l"
mkdir -p "$ROOT/build/tests"
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/user_process_transaction_phase2l_selftest.cpp" \
  "$ROOT/kernel/src/process/user_process_transaction.cpp" \
  -o "$OUT"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$OUT"
