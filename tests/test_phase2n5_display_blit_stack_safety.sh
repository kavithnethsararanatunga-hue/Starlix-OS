#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ABI="$ROOT/user_components/stlcs/stlcs_abi.hpp"
SERVICE="$ROOT/kernel/src/loader/user_host_service.cpp"
COMPONENT="$ROOT/user_components/stlcs/stlcs.cpp"
STACK="$ROOT/kernel/src/mem/kernel_stack.hpp"

for compiler in clang++ g++; do
  command -v "$compiler" >/dev/null
  echo "[$compiler] exception-frame policy ASan/UBSan"
  "$compiler" -std=c++23 -O1 -g -Wall -Wextra -Werror \
    -include "$ROOT/tests/freestanding_host_types.hpp" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/tests/exception_frame_policy_phase2n5_selftest.cpp" \
    -o "$TMP/exception_frame_policy_${compiler//+/p}"
  "$TMP/exception_frame_policy_${compiler//+/p}"
done

# Contract checks tie both sides of the ABI to the same bounded tile size.
grep -Fq 'DISPLAY_TILE_EDGE = 32u' "$ABI"
grep -Fq 'DISPLAY_TILE_MAX_PIXELS' "$ABI"
grep -Fq 'request.width > starlix::stlcs::DISPLAY_TILE_EDGE' "$SERVICE"
grep -Fq 'request.height > starlix::stlcs::DISPLAY_TILE_EDGE' "$SERVICE"
grep -Fq 'uint32_t pixels[DISPLAY_TILE_MAX]' "$SERVICE"
grep -Fq 'TILE_WIDTH = DISPLAY_TILE_EDGE' "$COMPONENT"
grep -Fq 'TILE_HEIGHT = DISPLAY_TILE_EDGE' "$COMPONENT"
# A four-page kernel stack is 16 KiB; DisplayBlit must use at most 4 KiB.
grep -Fq 'KERNEL_STACK_DEFAULT_PAGES = 4u' "$STACK"
grep -Fq 'sizeof(uint32_t) * DISPLAY_TILE_MAX <= 4096u' "$SERVICE"
echo '[PASS] DisplayBlit kernel scratch is capped at 4 KiB on a 16 KiB kernel stack'
echo '[PASS] User component and kernel HostApi share 32x32 tile dimensions'

echo 'PHASE2N5_DISPLAY_BLIT_STACK_SAFETY: PASS'
