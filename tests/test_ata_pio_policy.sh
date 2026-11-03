#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$ROOT/tests/ata_pio_policy_selftest.cpp" -o "$TMP/ata_pio_policy_selftest"
"$TMP/ata_pio_policy_selftest"
