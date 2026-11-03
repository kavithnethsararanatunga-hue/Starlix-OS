#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d /tmp/starlix-phase1c2-static.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

FLAGS=(
  -target x86_64-unknown-none
  -ffreestanding
  -fno-stack-protector
  -fno-exceptions
  -fno-rtti
  -mno-red-zone
  -mcmodel=large
  -std=c++26
  -O0 -g3
  -fno-inline
  -fno-omit-frame-pointer
  -fno-lto
  -Wall -Wextra
  -mgeneral-regs-only
)

clang++ "${FLAGS[@]}" -c \
  "$ROOT/kernel/src/cpu/idt.cpp" -o "$TMP/idt.o"

clang++ "${FLAGS[@]}" -c \
  "$ROOT/kernel/src/cpu/exception.cpp" -o "$TMP/exception.o"

clang++ "${FLAGS[@]}" -c \
  "$ROOT/kernel/src/cpu/gdt.cpp" -o "$TMP/gdt.o"

clang++ "${FLAGS[@]}" -c \
  "$ROOT/kernel/src/cpu/idt_asm.S" -o "$TMP/idt_asm.o"

if objdump -d "$TMP/exception.o" "$TMP/gdt.o" |
   grep -Eiq '%(xmm|ymm|zmm|mm[0-7])'; then
    echo "[FAIL] SIMD register use in exception diagnostic call path." >&2
    exit 1
fi
echo "[PASS] Exception diagnostic call path uses no FP/SIMD registers."

nm "$TMP/idt_asm.o" |
  grep -E ' T isr_exception_[0-9]+$' > "$TMP/stubs.txt"

COUNT="$(wc -l < "$TMP/stubs.txt")"
test "$COUNT" -eq 32

python3 - "$ROOT" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])

asm = (root / "kernel/src/cpu/idt_asm.S").read_text()
hdr = (root / "kernel/src/cpu/exception.hpp").read_text()
idt = (root / "kernel/src/cpu/idt.cpp").read_text()
gdt = (root / "kernel/src/cpu/gdt.cpp").read_text()
cpp = (root / "kernel/src/cpu/exception.cpp").read_text()
build = (root / "scripts/build_kernel.sh").read_text()
main = (root / "kernel/src/main.cpp").read_text()

entries = re.findall(
    r"^\s*ISR_(NOERR|ERROR)\s+(\d+)\s*$",
    asm,
    re.MULTILINE
)

no_error = {int(v) for kind, v in entries if kind == "NOERR"}
with_error = {int(v) for kind, v in entries if kind == "ERROR"}

expected_error = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30}

assert no_error | with_error == set(range(32))
assert not no_error & with_error
assert with_error == expected_error
assert 20 in no_error
print("[PASS] Exception error-code mapping.")

assert "exception_stubs[32]" in idt
assert "vector < 32" in idt
assert "exception_ist_for_vector(vector)" in idt
assert "address != reinterpret_cast<uint64_t>(exception_stubs[vector])" in idt
print("[PASS] IDT wires and verifies all 32 exception gates.")

for offset in (120, 128, 136, 144, 152, 160, 168):
    assert f"== {offset}" in hdr
assert "sizeof(ExceptionFrame) == 176" in hdr
print("[PASS] Normalized frame size and offsets.")

assert "ist4" in gdt
assert "exception_ist_stack" in gdt
assert "tss_exception_stack_contains" in gdt
print("[PASS] IST4 range and stack checks.")

assert "UART_POLL_LIMIT" in cpp or "uart_poll_budget" in cpp
assert "output_chars_left" in cpp
assert "cr2" in cpp
assert "FRAME_VALIDATION" in cpp
assert "if (f.vector < 32)" in cpp
assert "exception_halt" in cpp
print("[PASS] Bounded fatal exception diagnostics.")

assert "src/cpu/exception.cpp" in build
assert "build/cpu/exception.o" in build
assert "IST1-IST4" in main
print("[PASS] Kernel build and boot diagnostics integration.")
PY

echo "[PASS] Phase 1C.2 static validation complete."
