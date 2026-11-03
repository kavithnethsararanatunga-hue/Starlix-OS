#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$ROOT/tools/ect/build.sh"
bash "$ROOT/tools/lbr/build.sh"
echo "[✓] ECT + LBR tools built."
