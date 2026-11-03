#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/boot/x64/stbootmngr.efi/src/main.cpp"
FIXTURE="$ROOT/tests/stbootmngr_basesystem_selftest.cpp"
CHILD="$ROOT/tests/stbootmngr_phase11_child.cpp"
EXPORTER="$ROOT/tests/stbootmngr_phase11_fixture_export.cpp"
for f in "$MAIN" "$FIXTURE" "$CHILD" "$EXPORTER"; do test -f "$f" || { echo "FAIL: missing $f" >&2; exit 1; }; done
for marker in \
  'child returned EFI_SUCCESS (Phase 11 handoff test passed)' \
  'child returned EFI error (Phase 11 handoff test failed)'; do
  grep -Fq "$marker" "$MAIN" || { echo "FAIL: missing parent status marker: $marker" >&2; exit 1; }
done
grep -Fq 'STBOOT_FS_HANDOFF_VERSION' "$CHILD" || { echo 'FAIL: child does not validate handoff version' >&2; exit 1; }
grep -Fq '/System/Library/CoreServices/boot.efi' "$CHILD" || { echo 'FAIL: child path lookup absent' >&2; exit 1; }
grep -Fq '"PE\0\0"' "$CHILD" || { echo 'FAIL: child does not validate PE signature' >&2; exit 1; }
grep -Fq '#ifndef STBOOT_PHASE11_EXPORT_ONLY' "$FIXTURE" || { echo 'FAIL: fixture main guard missing' >&2; exit 1; }
python3 - "$MAIN" <<'PY_CHECK'
from pathlib import Path
import re
import sys

src = Path(sys.argv[1]).read_text()
out = list(src)
i = 0
state = "code"
quote = None

while i < len(src):
    c = src[i]
    n = src[i + 1] if i + 1 < len(src) else ""

    if state == "code":
        if c == "/" and n == "/":
            out[i] = out[i + 1] = " "
            i += 2
            state = "line_comment"
            continue
        if c == "/" and n == "*":
            out[i] = out[i + 1] = " "
            i += 2
            state = "block_comment"
            continue
        if c in ('"', "'"):
            quote = c
            out[i] = " "
            i += 1
            state = "literal"
            continue
        i += 1
        continue

    if state == "line_comment":
        if c == "\n":
            state = "code"
        else:
            out[i] = " "
        i += 1
        continue

    if state == "block_comment":
        if c == "*" and n == "/":
            out[i] = out[i + 1] = " "
            i += 2
            state = "code"
            continue
        if c != "\n":
            out[i] = " "
        i += 1
        continue

    # Ignore string and character literals too.
    if c == "\\":
        out[i] = " "
        if i + 1 < len(src):
            if src[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
        else:
            i += 1
        continue
    if c == quote:
        out[i] = " "
        state = "code"
    elif c != "\n":
        out[i] = " "
    i += 1

clean = "".join(out)
match = re.search(r"\bExitBootServices\s*\(", clean)
if match:
    line = clean.count("\n", 0, match.start()) + 1
    print(f"FAIL: actual ExitBootServices call detected near line {line}")
    raise SystemExit(1)

print("PASS: no ExitBootServices call in executable C++ code")
PY_CHECK
echo 'PASS: Phase 11 child contract, PE validation, StartImage status logging, exporter guard, no ExitBootServices call'
