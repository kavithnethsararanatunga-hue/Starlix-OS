#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSP_ROOT="$ROOT/ssp_root"
OUT="$ROOT/image/ssp.img"

rm -rf "$SSP_ROOT"
mkdir -p "$SSP_ROOT/Starlix/System/Boot"
mkdir -p "$SSP_ROOT/Starlix/System"

cp "$ROOT/boot/x64/rxload.efi/build/Rxload.efi" "$SSP_ROOT/Starlix/System/Boot/Rxload.efi"
cp "$ROOT/RxKernel1.0.ect" "$SSP_ROOT/Starlix/System/RxKernel1.0.ect"

"$ROOT/tools/mkssp/mkssp" "$SSP_ROOT" "$OUT" --size 128
echo "[✓] SSP ready with Rxload.efi + RxKernel1.0.ect"
ls -la "$SSP_ROOT/Starlix/System/"
