#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d /tmp/starlix-phys-alloc.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

g++ -std=c++20 -O0 -g3 -fno-inline -fno-lto \
    -Wall -Wextra -Werror \
    -I"$ROOT/kernel/src/mem" \
    "$ROOT/tests/test_phys_alloc_math.cpp" \
    -o "$TMP/test_phys_alloc_math"

"$TMP/test_phys_alloc_math"
