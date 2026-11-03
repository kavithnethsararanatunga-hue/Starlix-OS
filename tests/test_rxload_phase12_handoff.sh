#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/boot/x64/rxload.efi/src/main.cpp"
[[ -f "$SRC" ]] || { echo "FAIL: missing $SRC" >&2; exit 1; }
python3 - "$SRC" <<'PYTEST'
from pathlib import Path
import re, sys
s = Path(sys.argv[1]).read_text()
checks = {
    "STBOOT handoff header included": '#include "stboot_handoff.hpp"' in s,
    "Loaded-image LoadOptions inspected": "LoadOptionsSize" in s and "LoadOptions" in s,
    "handoff signature/version/flags checked": "STBOOT_FS_HANDOFF_VERSION" in s and "expected[8]" in s and "requiredFlags" in s,
    "kernel ECT path read via handoff": '/Starlix/System/RxKernel1.0.ect' in s and "ReadKernelEctFromHandoff" in s,
    "bounded chunked filesystem reads": "chunkLimit" in s and "handoff->Read" in s,
    "legacy SSP path retained": "SspMount(BS, &ssp)" in s and "SspReadFile(&ssp, kEntry, ectBuf)" in s,
    "APFS KEXT branch stages bundles": "PrepareKextModuleHandoff" in s and "ListDirectory" in s,
    "memory-map finalization helper": "FinalizeMemoryMapAndExitBootServices" in s,
    "fresh map retry on invalid key": "status != EFI_INVALID_PARAMETER" in s and "BS->GetMemoryMap" in s and "BS->ExitBootServices" in s,
}
for label, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + label)
if not all(checks.values()): raise SystemExit(1)
# Ensure the boot manager itself still has no actual ExitBootServices call.
bootmgr = Path(sys.argv[1]).parents[2] / "stbootmngr.efi/src/main.cpp"
if bootmgr.exists():
    text = bootmgr.read_text()
    # Remove C++ comments and literals to avoid false positives from diagnostic text.
    out = list(text); i=0; state='code'; quote=''
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': out[i]=out[i+1]=' '; i+=2; state='line'; continue
            if c=='/' and n=='*': out[i]=out[i+1]=' '; i+=2; state='block'; continue
            if c in ('"', "'"): quote=c; out[i]=' '; i+=1; state='literal'; continue
            i+=1; continue
        if state=='line':
            if c=='\n': state='code'
            else: out[i]=' '
            i+=1; continue
        if state=='block':
            if c=='*' and n=='/': out[i]=out[i+1]=' '; i+=2; state='code'; continue
            if c!='\n': out[i]=' '
            i+=1; continue
        if c=='\\':
            out[i]=' '
            if i+1<len(text):
                if text[i+1]!='\n': out[i+1]=' '
                i+=2
            else: i+=1
            continue
        if c==quote: out[i]=' '; state='code'
        elif c!='\n': out[i]=' '
        i+=1
    if re.search(r'\bExitBootServices\s*\(', ''.join(out)):
        raise SystemExit('FAIL: actual ExitBootServices call found in stbootmngr')
    print('PASS: stbootmngr has no executable ExitBootServices call')
PYTEST
