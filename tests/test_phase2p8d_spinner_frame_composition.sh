#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT/boot/x64/rxload.efi/src/boot_splash.hpp"
SELFTEST="$ROOT/tests/boot_splash_svg_selftest.cpp"
grep -Fq 'BootSplashComposeSpinnerFrame' "$HEADER"
grep -Fq 'BootSplashPresentSpinnerFrame' "$HEADER"
grep -Fq 'gBootSplashSpinnerScratch' "$HEADER"
grep -Fq 'if (dst[x] != src[x]) dst[x] = src[x];' "$HEADER"
if grep -Fq 'BootSplashFillRect(s,left-8,top-8,size+16,size+16)' "$HEADER"; then
  echo '[FAIL] Live framebuffer clear-before-draw path remains.' >&2
  exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for CXX in clang++ g++; do
  command -v "$CXX" >/dev/null
  "$CXX" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT" -I"$ROOT/include" -I"$ROOT/boot/x64/stbootmngr.efi/src" -I"$ROOT/kernel/src" \
    "$SELFTEST" -o "$TMP/boot-splash-$CXX"
  "$TMP/boot-splash-$CXX"
done
echo 'PHASE2P8D_SPINNER_FRAME_COMPOSITION: PASS (off-screen compose, delayed present, dirty-pixel writes; Clang/GCC ASan/UBSan)'
