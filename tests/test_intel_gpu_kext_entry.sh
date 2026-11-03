#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d /tmp/starlix-intel-kext-entry.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || { echo "Missing compiler: $CXX" >&2; exit 2; }
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I"$ROOT/include" \
    -I"$ROOT/kernel/src/drivers/Gpu/Basic/Intel" \
    "$ROOT/kernel/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp" \
    "$ROOT/tests/intel_gpu_kext_entry_probe.cpp" -o "$TMP/intel_gpu_kext_entry_probe"
"$TMP/intel_gpu_kext_entry_probe"
