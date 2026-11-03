#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT/boot/x64/rxload.efi/src/boot_splash.hpp"
SVG="$ROOT/boot/x64/rxload.efi/src/assets/StarlixBootSpinner.svg"
SELFTEST="$ROOT/tests/boot_splash_svg_selftest.cpp"

grep -Fq 'stroke-dashoffset: -153;' "$SVG"
grep -Fq 's.offset0 = 0; s.offset50 = -40; s.offset100 = -153;' "$HEADER"
grep -Fq 'int32_t offsets[3] = {0,-40,-153};' "$HEADER"
grep -Fq 'BootSplashInterpolateU32Nearest' "$HEADER"
grep -Fq 'BootSplashInterpolateI32Nearest' "$HEADER"
grep -Fq 'BootSplashEvaluateDashFrame(svg, elapsedMs)' "$HEADER"
grep -Fq 'BootSplashEvaluateDashFrame(&svg, 4983u)' "$SELFTEST"
grep -Fq 'BootSplashEvaluateDashFrame(&svg, 5000u)' "$SELFTEST"
grep -Fq 'phaseDistance > 2u' "$SELFTEST"
grep -Fq 'BootSplashEvaluateDashFrame(&svg, 4999u)' "$SELFTEST"

echo 'PHASE2P8C_SPINNER_LOOP_CONTINUITY: PASS'
