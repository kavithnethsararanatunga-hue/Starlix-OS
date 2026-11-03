#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/user_process_start_phase2m_selftest.cpp" \
  -o "$TMP/user_process_start_phase2m_selftest"
"$TMP/user_process_start_phase2m_selftest"
