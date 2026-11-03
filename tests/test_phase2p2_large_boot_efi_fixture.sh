#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_EFI="$ROOT/boot/x64/rxload.efi/build/boot.efi"
KERNEL_ECT="$ROOT/build/RxKernel1.0.phase2n2-production.ect"
for f in "$BOOT_EFI" "$KERNEL_ECT"; do
  [[ -s "$f" ]] || { echo "Missing required built artifact: $f (run bash scripts/build.sh and the Phase 2N candidate build first)" >&2; exit 1; }
done
BOOT_BYTES="$(stat -c %s "$BOOT_EFI")"
ECT_BYTES="$(stat -c %s "$KERNEL_ECT")"
(( BOOT_BYTES > 64 * 1024 && BOOT_BYTES <= 3 * 1024 * 1024 )) || {
  echo "ABORT: boot.efi must be >64 KiB and <=3 MiB; got $BOOT_BYTES bytes" >&2; exit 1;
}
(( ECT_BYTES > 152 && ECT_BYTES <= 4 * 1024 * 1024 )) || {
  echo "ABORT: kernel ECT size outside Phase 12 policy: $ECT_BYTES" >&2; exit 1;
}
TMP="$(mktemp -d "${TMPDIR:-/tmp}/starlix-phase2p2-large-boot.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
SYS="$TMP/example-kext-test.sys"
RXFS="$TMP/phase12-synthetic-rxfs-16m.img"
LOG="$TMP/fixture-export.log"
EXPORTER="$TMP/phase12_fixture_export"

bash "$ROOT/tools/sys/build_test_sys_fixture.sh" "$SYS"
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

if [[ -s "$ROOT/RXFS/sys/User/stlcs.ect" ]]; then
  "$EXPORTER" "$BOOT_EFI" "$KERNEL_ECT" "$SYS" "$RXFS" \
    "$ROOT/RXFS/sys/User/stlcs.ect" | tee "$LOG"
  grep -Fq 'PASS: RXFS /sys/User/stlcs.ect path lookup and byte-exact readback' "$LOG"
else
  "$EXPORTER" "$BOOT_EFI" "$KERNEL_ECT" "$SYS" "$RXFS" | tee "$LOG"
fi
grep -Fq 'PASS: complete boot.efi payload APFS readback matches source' "$LOG"
grep -Fq 'PASS: complete BaseSystem.dmg RXFS readback matches across indirect and double-indirect blocks' "$LOG"
[[ "$(stat -c %s "$RXFS")" -eq 16777216 ]]
echo "PHASE2P2_LARGE_BOOT_EFI_FIXTURE: PASS (boot.efi=${BOOT_BYTES} bytes; kernel.ect=${ECT_BYTES} bytes; APFS=8 MiB; RXFS=16 MiB)"
