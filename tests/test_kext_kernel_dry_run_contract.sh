#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/kernel/src/main.cpp"
RUNTIME="$ROOT/kernel/src/loader/kext_runtime.hpp"
DRYRUN="$ROOT/kernel/src/loader/kext_dry_run.hpp"

grep -Fq '#include "loader/kext_runtime.hpp"' "$MAIN"
grep -Fq 'load_external_gpu_kext(bootInfo, gpu_pci_scan_performed)' "$MAIN"
grep -Fq 'StarlixBootModuleKindKextDb' "$MAIN"
grep -Fq 'StarlixGpuKextEntry' "$MAIN"
grep -Fq 'entry(&host, &driver_api)' "$MAIN"
grep -Fq 'KextRuntimeLoadExternal(' "$MAIN"
grep -Fq 'KextRuntimeUnloadExternal(' "$MAIN"
grep -Fq 'managed_alloc_phys_pages' "$RUNTIME"
grep -Fq 'managed_release_phys_pages' "$RUNTIME"
grep -Fq 'RelocateSysElf64Image(' "$RUNTIME"
grep -Fq 'KERNEL_MODULE_MAP_EXECUTE' "$RUNTIME"
grep -Fq 'KERNEL_MODULE_MAP_WRITE' "$RUNTIME"
if grep -Fq '#include "loader/kext_dry_run.hpp"' "$MAIN"; then
    echo '[FAIL] Production main still uses the preflight-only dry-run loader.' >&2
    exit 1
fi
if grep -Eiq 'Intel|8086|0152|StarlixGpu' "$RUNTIME"; then
    echo '[FAIL] Vendor-specific identity leaked into generic runtime loader.' >&2
    exit 1
fi
if grep -Fq 'gKextDryRunImage' "$RUNTIME"; then
    echo '[FAIL] Static kernel BSS KEXT image buffer remains.' >&2
    exit 1
fi
python3 - "$MAIN" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
pool=s.find('managed_page_pool_init()')
load=s.find('load_external_gpu_kext(bootInfo, gpu_pci_scan_performed)')
if pool < 0 or load < 0 or load < pool:
    raise SystemExit('[FAIL] KEXT runtime load must occur after managed-page-pool initialization.')
if 'g_external_gpu_graphics_host' not in s:
    raise SystemExit('[FAIL] Graphics host ABI lifetime is not retained after driver entry.')
print('PASS: external KEXT runtime load follows generic memory initialization')
print('PASS: graphics host ABI object remains resident for later driver calls')
PY
# The old dry-run helper remains a test utility only; it must not be linked from
# production KernelMain. Its code still owns no Intel-specific policy.
grep -Fq 'RelocateSysElf64Image(' "$DRYRUN"
if grep -Eiq 'Intel|8086|0152|StarlixGpu' "$DRYRUN"; then
    echo '[FAIL] Intel-specific identity policy leaked into generic dry-run helper.' >&2
    exit 1
fi
echo 'KEXT_KERNEL_RUNTIME_LOADER_CONTRACT: PASS (external module mapped W^X; entry invoked via generic PCI visitor; dry-run is test-only)'
