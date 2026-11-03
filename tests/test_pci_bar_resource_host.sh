#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d /tmp/starlix-pci-bar-resource.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || { echo "Missing compiler: $CXX" >&2; exit 2; }
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT/include" "$ROOT/tests/pci_bar_probe_probe.cpp" \
    -o "$TMP/pci_bar_probe_probe"
"$TMP/pci_bar_probe_probe"
