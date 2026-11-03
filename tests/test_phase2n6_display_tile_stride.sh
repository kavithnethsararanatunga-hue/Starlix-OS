#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tests/phase2n6_tile_stride"
mkdir -p "$BUILD"

python3 - "$ROOT" <<'PY'
from pathlib import Path
import re, sys
root = Path(sys.argv[1])
src = (root / "user_components/stlcs/stlcs.cpp").read_text()
abi = (root / "user_components/stlcs/stlcs_abi.hpp").read_text()
rect_start = src.find("static void stlcs_tile_rect(")
rect_end = src.find("static stlcs_u32 stlcs_text_length(", rect_start)
if rect_start < 0 or rect_end < 0:
    raise SystemExit("FAIL: cannot locate stlcs_tile_rect body")
body = src[rect_start:rect_end]
if "display_tile_pixel_index(py - tile_y, px - tile_x)" not in body:
    raise SystemExit("FAIL: tile renderer does not use the shared ABI pixel index")
if re.search(r"\*\s*64u|<<\s*6", body):
    raise SystemExit("FAIL: stale 64-pixel tile stride remains in rectangle renderer")
if "DISPLAY_TILE_EDGE = 32u" not in abi or "row * DISPLAY_TILE_EDGE + column" not in abi:
    raise SystemExit("FAIL: shared ABI does not define a 32-pixel fixed tile stride")
if "request.source_stride = TILE_WIDTH;" not in src:
    raise SystemExit("FAIL: DisplayBlit source stride no longer matches fixed tile width")
print("[PASS] rectangle renderer uses shared 32-pixel tile indexing")
print("[PASS] no legacy 64-pixel stride in tile rectangle renderer")
print("[PASS] clipped edge tiles retain fixed 32-pixel DisplayBlit stride")
PY

run_test() {
  local compiler="$1"
  local name="$2"
  if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "[SKIP] $name unavailable: $compiler"
    return 0
  fi
  echo "[$name] tile-index bounds under ASan/UBSan"
  "$compiler" -std=c++20 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/tests/stlcs_tile_stride_phase2n6_selftest.cpp" \
    -o "$BUILD/tile_stride_${name,,}"
  "$BUILD/tile_stride_${name,,}"
}
run_test "clang++" "Clang"
run_test "g++" "GCC"

bash "$ROOT/scripts/build_stlcs.sh"
bash "$ROOT/tests/test_stlcs.sh"
echo "PHASE2N6_DISPLAY_TILE_STRIDE: PASS"
