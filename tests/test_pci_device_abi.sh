#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d /tmp/starlix-pci-device-abi.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || { echo "Missing compiler: $CXX" >&2; exit 2; }
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/tests/pci_device_abi_probe.cpp" -o "$TMP/pci_device_abi_probe"
"$TMP/pci_device_abi_probe"
