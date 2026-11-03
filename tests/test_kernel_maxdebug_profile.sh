#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/scripts/build_kernel.sh"
[[ -f "$BUILD" ]] || { echo "Missing scripts/build_kernel.sh" >&2; exit 1; }

for flag in -O0 -g3 -gdwarf-5 -fstandalone-debug -fno-inline \
            -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-lto \
            -fno-merge-all-constants \
            -fno-vectorize -fno-slp-vectorize \
            -fno-function-sections -fno-data-sections -mgeneral-regs-only; do
  grep -Fq -- "$flag" "$BUILD" || { echo "[FAIL] missing $flag"; exit 1; }
done

if grep -Eq -- '(^|[[:space:]])-O(1|2|3|s|z|fast)([[:space:]]|$)' "$BUILD"; then
  echo "[FAIL] size/speed optimization flag found" >&2; exit 1
fi
for flag in --no-gc-sections --icf=none --build-id=sha1 -Map=; do
  grep -Fq -- "$flag" "$BUILD" || { echo "[FAIL] missing linker control $flag"; exit 1; }
done
if grep -Eq '(^|[[:space:]])(llvm-)?strip([[:space:]]|$)|--strip-(all|debug)' "$BUILD"; then
  echo "[FAIL] strip command/option found" >&2; exit 1
fi
grep -Fq 'kernel.symbols.txt' "$BUILD"
grep -Fq 'kernel.sections.txt' "$BUILD"
grep -Fq '.debug_info' "$BUILD"

tmp="$(mktemp -d /tmp/starlix-maxdebug-profile.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/probe.cpp" <<'CPP'
#define STARLIX_DEBUG_PROFILE_PROBE 1
struct StarlixDebugProbeType { unsigned long a; char label[32]; };
static const StarlixDebugProbeType kProbe{0x1234, "Starlix complete debug profile"};
__attribute__((used, noinline)) unsigned long starlix_debug_probe(unsigned long x) {
    return x + kProbe.a + sizeof(kProbe.label);
}
CPP
FLAGS="-target x86_64-unknown-none -ffreestanding -fno-stack-protector -mgeneral-regs-only -fno-exceptions -fno-rtti -mno-red-zone -mcmodel=large -std=c++26 -O0 -g3 -gdwarf-5 -fstandalone-debug -fno-inline -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-lto -fno-merge-all-constants -fno-vectorize -fno-slp-vectorize -fno-function-sections -fno-data-sections -Wall -Wextra"
# shellcheck disable=SC2086
clang++ $FLAGS -c "$tmp/probe.cpp" -o "$tmp/probe.o"
readelf -SW "$tmp/probe.o" | grep -q '\.debug_info' || { echo "[FAIL] object lacks DWARF info"; exit 1; }
cat > "$tmp/link.ld" <<'LD'
SECTIONS {
  . = 0x200000;
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(COMMON) *(.bss .bss.*) }
  /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
LD
ld.lld -nostdlib -static --no-gc-sections --icf=none --build-id=sha1 \
  -Map="$tmp/probe.map" -T "$tmp/link.ld" -o "$tmp/probe.elf" "$tmp/probe.o"
test -s "$tmp/probe.map"
readelf -SW "$tmp/probe.elf" | grep -q '\.debug_info' || { echo "[FAIL] linked ELF lost DWARF info"; exit 1; }
echo "PASS: no-size-optimization flags, retained DWARF, no strip/GC/ICF, link-map generation"
