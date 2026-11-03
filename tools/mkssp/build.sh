#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "[*] Building mkssp (host tool) ..."
g++ -std=c++20 -O2 -I"$ROOT/include" "$ROOT/tools/mkssp/mkssp.cpp" -o "$ROOT/tools/mkssp/mkssp"
echo "[+] mkssp built: $ROOT/tools/mkssp/mkssp"
