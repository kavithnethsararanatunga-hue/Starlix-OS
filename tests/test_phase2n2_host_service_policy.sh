#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/tests/user_host_service_policy_selftest.cpp"
for CXX in clang++ g++; do
    command -v "$CXX" >/dev/null 2>&1 || continue
    OUT="${TMPDIR:-/tmp}/starlix-phase2n2-host-service-${CXX//+/p}-$$"
    trap 'rm -f "$OUT"' EXIT
    echo "[$CXX] restricted HostApi policy ASan/UBSan"
    "$CXX" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -fno-omit-frame-pointer "$SRC" -o "$OUT"
    "$OUT"
    rm -f "$OUT"
    trap - EXIT
    echo "PASS: $CXX restricted HostApi policy"
done
