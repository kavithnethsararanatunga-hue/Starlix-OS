#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for cmd in clang clang++ python3; do
  command -v "$cmd" >/dev/null || {
    echo "ABORT: missing tool: $cmd" >&2
    exit 1
  }
done

TMP="$(mktemp -d /tmp/starlix-sys-format.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/test_driver.c" <<'C'
__attribute__((used))
void StarlixTestDriverEntry(void) {
    /* Test-only ELF symbol. This object is never executed. */
}
C

clang -target x86_64-unknown-elf -ffreestanding \
  -fno-stack-protector -fno-pic \
  -c "$TMP/test_driver.c" -o "$TMP/test_driver.o"

clang++ -std=c++20 -O0 -g3 \
  -I"$ROOT/include" \
  "$ROOT/tools/sys/pack_sys.cpp" -o "$TMP/pack_sys"

"$TMP/pack_sys" "$TMP/test_driver.o" "$TMP/valid.sys" \
  "StarlixTest" "StarlixTestDriverEntry"

python3 "$ROOT/tools/sys/validate_sys.py" "$TMP/valid.sys"

clang++ -std=c++20 -O0 -g3 -I"$ROOT/include" \
  "$ROOT/tests/sys_validate_probe.cpp" -o "$TMP/sys_validate_probe"
"$TMP/sys_validate_probe" "$TMP/valid.sys"

python3 - "$TMP" <<'PY'
from pathlib import Path
import struct
import sys

root = Path(sys.argv[1])
good = (root / "valid.sys").read_bytes()

cases = {}
bad = bytearray(good)
bad[0] ^= 1
cases["bad-magic.sys"] = bad

bad = bytearray(good)
struct.pack_into("<I", bad, 16, 0xBEEF)
cases["bad-architecture.sys"] = bad

bad = bytearray(good)
struct.pack_into("<Q", bad, 24, 1)
cases["bad-payload-size.sys"] = bad

bad = bytearray(good)
bad[104 + 10] ^= 0x20
cases["bad-crc.sys"] = bad

def refresh_payload_crc(package):
    import zlib
    struct.pack_into("<I", package, 32,
                     zlib.crc32(package[104:]) & 0xFFFFFFFF)

bad = bytearray(good)
bad[104 + 4] = 1
refresh_payload_crc(bad)
cases["bad-elf-class.sys"] = bad

bad = bytearray(good)
struct.pack_into("<H", bad, 104 + 16, 2)
refresh_payload_crc(bad)
cases["bad-elf-type.sys"] = bad

bad = bytearray(good)
struct.pack_into("<H", bad, 104 + 18, 3)
refresh_payload_crc(bad)
cases["bad-elf-machine.sys"] = bad

bad = bytearray(good)
struct.pack_into("<Q", bad, 104 + 40, 0xFFFFFFFFFFFFFFF0)
refresh_payload_crc(bad)
cases["bad-section-table-offset.sys"] = bad

bad = bytearray(good)
struct.pack_into("<H", bad, 104 + 58, 1)
refresh_payload_crc(bad)
cases["bad-section-entry-size.sys"] = bad

bad = bytearray(good)
missing = b"NoSuchEntry"
bad[64:96] = missing + bytes(32 - len(missing))
cases["missing-entry-symbol.sys"] = bad

(root / "truncated.sys").write_bytes(good[:-1])
for name, data in cases.items():
    (root / name).write_bytes(data)
PY

for bad in \
  "$TMP/bad-magic.sys" \
  "$TMP/bad-architecture.sys" \
  "$TMP/bad-payload-size.sys" \
  "$TMP/bad-crc.sys" \
  "$TMP/truncated.sys" \
  "$TMP/bad-elf-class.sys" \
  "$TMP/bad-elf-type.sys" \
  "$TMP/bad-elf-machine.sys" \
  "$TMP/bad-section-table-offset.sys" \
  "$TMP/bad-section-entry-size.sys" \
  "$TMP/missing-entry-symbol.sys"
do
  if python3 "$ROOT/tools/sys/validate_sys.py" "$bad"; then
    echo "FAIL: Python validator accepted invalid package: $bad" >&2
    exit 1
  fi
  if "$TMP/sys_validate_probe" "$bad"; then
    echo "FAIL: C++ validator accepted invalid package: $bad" >&2
    exit 1
  fi
  echo "PASS: Python and C++ validators rejected $(basename "$bad")"
done

echo
echo "PASS: SYS host validation and negative tests."
echo "No driver payload was executed."
