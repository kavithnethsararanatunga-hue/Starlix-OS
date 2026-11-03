#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/boot/x64/stbootmngr.efi/src/main.cpp"
for marker in \
  'Starlix boot manager: RXFS-first Phase 9' \
  '[RXFS] Scanning UEFI disk handles' \
  '[RXFS] Volume mounted read-only' \
  '[RXFS] Mount failed; trying legacy SSP fallback' \
  '[BaseSystem] Open failed; trying legacy SSP fallback' \
  '[RXFS] Starting boot.efi with read-only filesystem handoff' \
  '[BOOT] Using transitional legacy SSP fallback'; do
  grep -Fq "$marker" "$SRC" || { echo "FAIL: missing diagnostic marker: $marker"; exit 1; }
done
# Ensure read-only phase does not add an ExitBootServices transition.
if grep -Eq '[[:alnum:]_]+->ExitBootServices[[:space:]]*\(' "$SRC"; then
  echo "FAIL: unexpected ExitBootServices call in stbootmngr main.cpp"
  exit 1
fi
echo "PASS: Phase 9 console diagnostics and fallback markers present; no ExitBootServices call"
