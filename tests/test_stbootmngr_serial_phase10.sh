#!/usr/bin/env bash
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
