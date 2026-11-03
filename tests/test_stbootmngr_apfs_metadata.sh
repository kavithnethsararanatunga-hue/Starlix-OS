#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp "${TMPDIR:-/tmp}/starlix-apfs-metadata-selftest.XXXXXX")"
trap 'rm -f "$OUT"' EXIT
clang++ -D__STDC_HOSTED__=0 -Wno-builtin-macro-redefined -std=c++2c \
  -fno-exceptions -fno-rtti -O0 -g3 -Wall -Wextra \
  -I"$ROOT" -I"$ROOT/include" \
  "$ROOT/tests/stbootmngr_apfs_metadata_selftest.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_metadata_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_zlib_inflate.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/rxfs_reader.cpp" \
  -o "$OUT"
"$OUT"
echo "PASS: APFS Fletcher-64 checksum + container OMAP + volume APSB + volume OMAP + root-tree OID resolution"
