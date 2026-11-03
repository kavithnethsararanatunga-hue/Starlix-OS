#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for cmd in clang++ python3 stat grep; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 2; }
done
STLCS=""
for candidate in "$ROOT/RXFS/sys/User/stlcs.ect" "$ROOT/rxfs/sys/User/stlcs.ect"; do
  if [[ -s "$candidate" ]]; then STLCS="$candidate"; break; fi
done
[[ -n "$STLCS" ]] || { echo "ABORT: production RXFS/sys/User/stlcs.ect not found" >&2; exit 1; }
TMP="$(mktemp -d /tmp/starlix-phase2n1-rxfs.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# This isolated host fixture test checks namespace and exact bytes. The
# synthetic boot.efi is test data, not a bootable EFI image.
python3 - "$TMP/boot.efi" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b"PHASE2N1-BOOT-EFI-FIXTURE" + bytes(4096 - 25))
PY
python3 "$ROOT/tools/build_stbootmngr_phase12_test_ect.py" "$TMP/kernel.ect" >/dev/null
bash "$ROOT/tools/sys/build_test_sys_fixture.sh" "$TMP/test.sys" >/dev/null

EXPORTER="$TMP/phase2n1_fixture_export"
clang++ -D__STDC_HOSTED__=0 -Wno-builtin-macro-redefined \
  -std=c++2c -fno-exceptions -fno-rtti -O0 -g0 -Wall -Wextra \
  -I"$ROOT" -I"$ROOT/include" \
  "$ROOT/tests/stbootmngr_phase12_fixture_export.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/base_system_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/stboot_handoff.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_file_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_path_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_metadata_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_zlib_inflate.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/rxfs_reader.cpp" \
  -o "$EXPORTER"

OUT="$TMP/phase2n1-rxfs-16m.img"
"$EXPORTER" "$TMP/boot.efi" "$TMP/kernel.ect" "$TMP/test.sys" \
  "$OUT" "$STLCS" | tee "$TMP/export.log"
grep -Fq "PASS: RXFS /sys/User/stlcs.ect path lookup and byte-exact readback" "$TMP/export.log"
[[ "$(stat -c %s "$OUT")" -eq 16777216 ]]
echo "PASS: Phase 2N.1 fixture embeds and verifies the actual stlcs.ect at /sys/User/stlcs.ect."
sha256sum "$STLCS" "$OUT"
