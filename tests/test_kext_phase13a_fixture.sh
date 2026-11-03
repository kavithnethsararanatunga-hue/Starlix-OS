#!/usr/bin/env bash
set -euo pipefail
# STARLIX_PHASE13A_FIXTURE_HOST_TEST
# Builds a synthetic RXFS -> UDIF -> APFS image in /tmp and validates the
# example.kext Info.plist and non-executed .sys payload via APFS readback.
# This does not create a VM candidate or touch any physical/EFI system disk.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 1 ]]; then
  echo "Usage: bash tests/test_kext_phase13a_fixture.sh <production-kernel.ect>" >&2
  exit 2
fi
BOOT_EFI="$ROOT/boot/x64/rxload.efi/build/boot.efi"
ECT="$1"
[[ -s "$BOOT_EFI" ]] || { echo "ABORT: built boot.efi not found: $BOOT_EFI" >&2; exit 1; }
[[ -s "$ECT" ]] || { echo "ABORT: production ECT not found: $ECT" >&2; exit 1; }
for cmd in clang clang++ stat sha256sum; do
  command -v "$cmd" >/dev/null || { echo "ABORT: required tool missing: $cmd" >&2; exit 1; }
done
ECT_BYTES="$(stat -c %s "$ECT")"
if (( ECT_BYTES <= 152 || ECT_BYTES > 768 * 1024 )); then
  echo "ABORT: ECT must be >152 bytes and <=768 KiB; got $ECT_BYTES" >&2
  exit 1
fi
TMP="$(mktemp -d /tmp/starlix-kext-phase13a.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

DB_SYS="$TMP/example-kext-test.sys"
bash "$ROOT/tools/sys/build_test_sys_fixture.sh" "$DB_SYS"

EXPORTER="$TMP/phase13a_fixture_export"
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
OUT="$TMP/phase13a-synthetic-rxfs-4m.img"
"$EXPORTER" "$BOOT_EFI" "$ECT" "$DB_SYS" "$OUT"
[[ "$(stat -c %s "$OUT")" -eq 4194304 ]] || {
  echo "ABORT: synthetic RXFS fixture did not have expected 4 MiB size" >&2
  exit 1
}
echo "PASS: synthetic STSYS001 .sys payload verified via APFS readback; not executed."
sha256sum "$OUT"
