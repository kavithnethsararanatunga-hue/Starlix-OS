#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/starlix-rxfs-native-selftest"
CXX="${CXX:-clang++}"
"$CXX" -std=c++26 -O0 -g3 -Wall -Wextra -Werror \
  "$ROOT/tests/rxfs_native_selftest.cpp" -o "$OUT"
"$OUT" "$@"
