#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PYTEST'
from pathlib import Path
import sys
r=Path(sys.argv[1])
apfs_h=(r/"boot/x64/stbootmngr.efi/src/apfs_path_reader.hpp").read_text()
apfs_cpp=(r/"boot/x64/stbootmngr.efi/src/apfs_path_reader.cpp").read_text()
hh=(r/"boot/x64/stbootmngr.efi/src/stboot_handoff.hpp").read_text()
hc=(r/"boot/x64/stbootmngr.efi/src/stboot_handoff.cpp").read_text()
rx=(r/"boot/x64/rxload.efi/src/main.cpp").read_text()
ki=(r/"kernel/src/bootinfo.hpp").read_text()
km=(r/"kernel/src/main.cpp").read_text()
checks={
 "APFS directory enumeration API": "ApfsEnumerateDirectory" in apfs_h and "ApfsEnumerateDirectory" in apfs_cpp,
 "bounded APFS traversal": "kDirectoryMaxNodes" in apfs_cpp and "DirectoryMarkSeen" in apfs_cpp,
 "versioned v2 read-only handoff": "STBFS002" in hh and "STBOOT_FS_HANDOFF_VERSION = 2" in hh and "ListDirectory" in hh,
 "handoff enumeration adapter": "HandoffListDirectory" in hc and "ApfsEnumerateDirectory" in hc,
 "KEXT SYS payload path": "/Contents/Starlix/" in rx and "KextPlistGetStarlixExecutable" in rx,
 "Info.plist executable metadata": "KextPlistLooksValid" in rx and "KextPlistGetStarlixExecutable" in rx and "/Contents/Info.plist" in rx,
 "bounded DB size": "kMaxKextDbBytes" in rx and "kMaxTotalKextDbBytes" in rx,
 "BootInfo KEXT module kind": "StarlixBootModuleKindKextDb" in ki,
 "KEXT table bound matches loader capacity": "SystemModuleCount <= 256" in km and "256U * sizeof(StarlixBootModule)" in rx, # STARLIX_PHASE13A_CHECK_TABLE_BOUND
 "kernel invokes external KEXT runtime after prerequisites": "load_external_gpu_kext(bootInfo, gpu_pci_scan_performed)" in km and "KextRuntimeLoadExternal(" in km and "StarlixGpuKextEntry EXECUTED" in km,
}
for name,ok in checks.items():
 print(("PASS" if ok else "FAIL")+": "+name)
if not all(checks.values()): raise SystemExit(1)
PYTEST
