#!/usr/bin/env python3
"""Safely correct the Phase 10 UEFI Serial I/O ABI and false-positive test."""
from pathlib import Path
from datetime import datetime
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "boot/x64/stbootmngr.efi/src/main.cpp"
TEST = ROOT / "tests/test_stbootmngr_serial_phase10.sh"

old_struct = '''struct STBOOT_SERIAL_IO_PROTOCOL;
using STBOOT_SERIAL_FN_RESET = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*);
using STBOOT_SERIAL_FN_SET_ATTRIBUTES = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t);
using STBOOT_SERIAL_FN_SET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t);
using STBOOT_SERIAL_FN_GET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t*);
using STBOOT_SERIAL_FN_TRANSFER = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, UINTN*, void*);
struct STBOOT_SERIAL_IO_PROTOCOL {
    STBOOT_SERIAL_FN_RESET Reset;
    STBOOT_SERIAL_FN_SET_ATTRIBUTES SetAttributes;
    STBOOT_SERIAL_FN_SET_CONTROL SetControl;
    STBOOT_SERIAL_FN_GET_CONTROL GetControl;
    STBOOT_SERIAL_FN_TRANSFER Write;
    STBOOT_SERIAL_FN_TRANSFER Read;
    void* Mode;
    uint32_t Revision;
};'''

new_struct = '''struct STBOOT_SERIAL_IO_PROTOCOL;
using STBOOT_SERIAL_FN_RESET = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*);
// EFI_PARITY_TYPE and EFI_STOP_BITS_TYPE are 32-bit UEFI enums; DataBits is UINT8.
using STBOOT_SERIAL_FN_SET_ATTRIBUTES = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t);
using STBOOT_SERIAL_FN_SET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t);
using STBOOT_SERIAL_FN_GET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t*);
using STBOOT_SERIAL_FN_TRANSFER = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, UINTN*, void*);
// Field order follows EFI_SERIAL_IO_PROTOCOL in the UEFI Specification.
struct STBOOT_SERIAL_IO_PROTOCOL {
    uint32_t Revision;
    STBOOT_SERIAL_FN_RESET Reset;
    STBOOT_SERIAL_FN_SET_ATTRIBUTES SetAttributes;
    STBOOT_SERIAL_FN_SET_CONTROL SetControl;
    STBOOT_SERIAL_FN_GET_CONTROL GetControl;
    STBOOT_SERIAL_FN_TRANSFER Write;
    STBOOT_SERIAL_FN_TRANSFER Read;
    void* Mode;
};'''

new_test = r'''#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/boot/x64/stbootmngr.efi/src/main.cpp"
test -f "$MAIN"
for marker in \
  'STBOOT_SERIAL_IO_PROTOCOL' \
  'BB25CF6F' \
  'RXFS mount attempt' \
  'RXFS mount PASS' \
  'RXFS mount FAIL' \
  'BaseSystem/APFS open attempt' \
  'boot.efi bytes read PASS' \
  'UEFI LoadImage PASS' \
  'RXFS-first path returned; attempting SSP fallback'; do
  grep -Fq "$marker" "$MAIN" || { echo "FAIL: missing serial marker: $marker" >&2; exit 1; }
done
python3 - "$MAIN" <<'PY'
from pathlib import Path
import re, sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
# Ignore C/C++ comments when looking for an actual function call.
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
text = re.sub(r"//[^\n]*", "", text)
if re.search(r"\bExitBootServices\s*\(", text):
    raise SystemExit("FAIL: unexpected ExitBootServices call")

struct_start = text.find("struct STBOOT_SERIAL_IO_PROTOCOL {")
if struct_start < 0:
    raise SystemExit("FAIL: serial protocol struct definition missing")
struct_end = text.find("};", struct_start)
struct_text = text[struct_start:struct_end]
fields = ["uint32_t Revision;", "STBOOT_SERIAL_FN_RESET Reset;",
          "STBOOT_SERIAL_FN_SET_ATTRIBUTES SetAttributes;",
          "STBOOT_SERIAL_FN_SET_CONTROL SetControl;",
          "STBOOT_SERIAL_FN_GET_CONTROL GetControl;",
          "STBOOT_SERIAL_FN_TRANSFER Write;",
          "STBOOT_SERIAL_FN_TRANSFER Read;", "void* Mode;"]
positions = [struct_text.find(f) for f in fields]
if any(p < 0 for p in positions) or positions != sorted(positions):
    raise SystemExit("FAIL: EFI_SERIAL_IO_PROTOCOL field order is not UEFI-compatible")
if not re.search(r"STBOOT_SERIAL_FN_SET_ATTRIBUTES\s*=\s*EFI_STATUS\s*\(ABI\s*\*\)\s*\(STBOOT_SERIAL_IO_PROTOCOL\*[,]\s*uint64_t[,]\s*uint32_t[,]\s*uint32_t[,]\s*uint32_t[,]\s*uint8_t[,]\s*uint32_t\s*\);", text):
    raise SystemExit("FAIL: UEFI Serial SetAttributes ABI signature is incorrect")
print("PASS: UEFI Serial I/O ABI field order/signatures; comments ignored for ExitBootServices check")
PY
echo 'PASS: serial diagnostics markers present; no ExitBootServices call'
'''

if not MAIN.is_file() or not TEST.is_file():
    raise SystemExit("ABORT: expected Phase 10 main.cpp and test script are missing")
main_text = MAIN.read_text(encoding="utf-8")
test_text = TEST.read_text(encoding="utf-8")
if main_text.count(old_struct) != 1:
    raise SystemExit("ABORT: expected outdated serial protocol struct was not found exactly once")
if not re.search(r"if grep -Eq .*ExitBootServices.*then", test_text):
    raise SystemExit("ABORT: expected false-positive ExitBootServices test check was not found")

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
for path in (MAIN, TEST):
    backup = path.with_name(path.name + f".bak-phase10-abi-fix-{stamp}")
    backup.write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"Backup : {backup.relative_to(ROOT)}")

MAIN.write_text(main_text.replace(old_struct, new_struct, 1), encoding="utf-8")
TEST.write_text(new_test, encoding="utf-8")
TEST.chmod(TEST.stat().st_mode | 0o111)
print(f"Updated: {MAIN.relative_to(ROOT)}")
print(f"Updated: {TEST.relative_to(ROOT)}")
print("Corrected UEFI Serial I/O protocol field order and SetAttributes ABI; fixed comment false-positive.")
