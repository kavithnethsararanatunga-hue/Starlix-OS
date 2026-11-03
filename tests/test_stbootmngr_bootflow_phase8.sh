#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
main = (root / "boot/x64/stbootmngr.efi/src/main.cpp").read_text()
build = (root / "scripts/build.sh").read_text()
abi = (root / "boot/x64/stbootmngr.efi/src/stboot_handoff.hpp").read_text()
checks = [
    ('RXFS mount attempted first', main.index('TryRxfsBootEfi(ImageHandle, BS)') < main.index('RunLegacySspFallback(ImageHandle, BS)')),
    ('BaseSystem resolver called', 'StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem)' in main),
    ('boot.efi loaded from bytes', 'BS->LoadImage(FALSE, ImageHandle, nullptr, imageBuffer' in main),
    ('handoff passed through LoadOptions', 'childLoadedImage->LoadOptions = &handoff' in main),
    ('StartImage occurs after LoadOptions set', main.index('childLoadedImage->LoadOptions = &handoff') < main.index('BS->StartImage(childImage, nullptr, nullptr)')),
    ('legacy SSP fallback retained', 'SspMount(BS, &ssp)' in main),
    ('versioned read-only handoff contract', 'STBOOT_FS_HANDOFF_VERSION = 1' in abi and 'STBOOT_FS_HANDOFF_READ_ONLY' in abi),
    ('handoff object linked', 'stboot_handoff.o' in build),
]
for label, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + label)
if not all(ok for _, ok in checks):
    raise SystemExit(1)
PY
