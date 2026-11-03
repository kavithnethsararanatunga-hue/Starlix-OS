#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CXX="${CXX:-clang++}"

"$CXX" -std=c++23 -O1 -g -Wall -Wextra -Werror \
    -include "$ROOT/tests/freestanding_host_types.hpp" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/tests/mmio_phase9_policy_selftest.cpp" \
    -o "$TMP/mmio_phase9a_policy_selftest"

"$TMP/mmio_phase9a_policy_selftest"

echo "MMIO_PHASE9A_POLICY_TEST: PASS"
