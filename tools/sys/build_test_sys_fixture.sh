#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <output.sys>" >&2
  exit 2
fi

OUT="$1"
mkdir -p "$(dirname "$OUT")"
TMP="$(mktemp -d /tmp/starlix-test-sys.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

for cmd in clang clang++ python3; do
  command -v "$cmd" >/dev/null || {
    echo "ABORT: required tool missing: $cmd" >&2
    exit 1
  }
done

cat > "$TMP/test_driver.c" <<'C'
__attribute__((used))
void StarlixTestDriverEntry(void) {
    /* Test fixture only. This function is never executed. */
}
C

clang -target x86_64-unknown-elf -ffreestanding \
  -fno-stack-protector -fno-pic \
  -c "$TMP/test_driver.c" -o "$TMP/test_driver.o"

clang++ -std=c++20 -O0 -g3 \
  -I"$ROOT/include" \
  "$ROOT/tools/sys/pack_sys.cpp" -o "$TMP/pack_sys"

"$TMP/pack_sys" "$TMP/test_driver.o" "$TMP/example.sys" \
  "ExampleKext" "StarlixTestDriverEntry"

python3 "$ROOT/tools/sys/validate_sys.py" "$TMP/example.sys"

cp -- "$TMP/example.sys" "$OUT.tmp"
mv -- "$OUT.tmp" "$OUT"

echo "PASS: valid, non-executed SYS test package created: $OUT"
