#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/user_components/stlcs"
BUILD="$ROOT/build/user_components/stlcs"
OUT="$ROOT/RXFS/sys/User/stlcs.ect"
CXX="${CXX:-clang++}"
LD="${LD:-ld.lld}"

command -v "$CXX" >/dev/null 2>&1 || { echo "[ERROR] C++ compiler not found: $CXX" >&2; exit 1; }
command -v "$LD" >/dev/null 2>&1 || { echo "[ERROR] LLVM linker not found: $LD" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "[ERROR] python3 not found" >&2; exit 1; }
mkdir -p "$BUILD" "$(dirname "$OUT")"

# Keep debug information and O0 by design. The ELF sidecar is retained for
# diagnosis; the RXFS ECT contains only mapped loadable sections.
# The ECT loader maps each section at PREFERRED_BASE + section_vaddr.
# Keep all image-local references position-independent and non-interposable.
CXXFLAGS=(
  --target=x86_64-unknown-none-elf
  -std=c++20 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto
  -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
  -fpie -fvisibility=hidden -fno-unwind-tables -fno-asynchronous-unwind-tables
  -fno-use-cxa-atexit -fno-threadsafe-statics -mno-red-zone
  -mgeneral-regs-only -Wall -Wextra -Werror
)

echo "[*] Compiling Starlix Lock Screen system component..."
"$CXX" "${CXXFLAGS[@]}" -c "$SRC/stlcs.cpp" -o "$BUILD/stlcs.o"

echo "[*] Linking freestanding x86-64 component ELF (debug symbols retained)..."
"$LD" -m elf_x86_64 -nostdlib -static --build-id=none \
  -T "$SRC/stlcs.ld" -o "$BUILD/stlcs.elf" "$BUILD/stlcs.o"

# Reject unexpected writable zero-fill state before converting to ECT v1.
if readelf -S "$BUILD/stlcs.elf" | grep -Eq '\.bss[[:space:]]+NOBITS[[:space:]]+0*[1-9A-Fa-f]'; then
  echo "[ERROR] stlcs ECT v1 currently cannot encode non-empty .bss." >&2
  exit 1
fi

# Fail the build if compiler output regresses to absolute low-address data refs.
python3 "$SRC/validate_pic.py" "$BUILD/stlcs.elf"
python3 "$SRC/pack_ect.py" "$BUILD/stlcs.elf" "$OUT"
python3 "$SRC/validate_ect.py" "$OUT"
sha256sum "$OUT" > "$BUILD/stlcs.ect.sha256"

echo "[✓] Built $OUT"
echo "[i] Debug ELF retained at $BUILD/stlcs.elf"
