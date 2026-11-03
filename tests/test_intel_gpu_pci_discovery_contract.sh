#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER="$ROOT/kernel/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp"
POLICY="$ROOT/kernel/src/drivers/Gpu/Basic/Intel/intel_gpu_pci_policy.hpp"
KERNEL_BUILD="$ROOT/scripts/build_kernel.sh"
KEXT_BUILD="$ROOT/scripts/build_intlgpudvs.sh"
PCI_HPP="$ROOT/kernel/src/drivers/pci.hpp"
PCI_CPP="$ROOT/kernel/src/drivers/pci.cpp"
MAIN_CPP="$ROOT/kernel/src/main.cpp"

fail() { echo "FAIL: $1" >&2; exit 1; }
pass() { echo "PASS: $1"; }

[[ -s "$DRIVER" ]] || fail "Intel driver source missing at required path"
[[ -s "$POLICY" ]] || fail "Intel KEXT policy missing from driver folder"
pass "Intel driver source resides in kernel/src/drivers/Gpu/Basic/Intel"

grep -Fq 'SRC="$KDIR/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp"' "$KEXT_BUILD" \
    || fail "KEXT build script does not compile the requested external source"
pass "External KEXT build script compiles the dedicated Intel source"

if grep -Eq 'Gpu/Basic/Intel|intlgpudvs\.cpp|intlgpudvs\.o' "$KERNEL_BUILD"; then
    fail "kernel build script references Intel driver source/object"
fi
pass "Kernel build script does not compile or link Intel KEXT source"

if grep -Eq 'intel_gpu_pci_policy|IntelDisplayPciInfo|IsIntelDisplayController|IsSupportedKextDevice|INTEL-GPU' \
    "$PCI_HPP" "$PCI_CPP" "$MAIN_CPP"; then
    fail "Intel-specific device matching remains in generic kernel PCI/main path"
fi
pass "Kernel PCI enumeration remains vendor-neutral"

if grep -Eq 'StarlixIntelGpuMatchesPciId|IsSupportedKextDevice' "$DRIVER"; then
    pass "PCI identity matching is implemented by the external Intel driver"
else
    fail "External driver does not own PCI identity matching"
fi

if grep -Fq 'StarlixGpuKextEntry' "$DRIVER" && \
   grep -Fq 'StarlixGpuKextEntry' "$KEXT_BUILD"; then
    pass "External KEXT exports and packages the generic PCI-context entry"
else
    fail "External KEXT entry ABI is missing from source or package script"
fi

if grep -Fq 'pci_query_device' "$PCI_HPP" && \
   grep -Fq 'pci_query_device' "$PCI_CPP"; then
    pass "Generic kernel PCI subsystem exposes a read-only device snapshot"
else
    fail "Generic PCI device snapshot API is missing"
fi

python3 - "$PCI_CPP" <<'PYPCI'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.find('bool pci_query_device(')
end = s.find('\n}\n', start)
if start < 0 or end < 0:
    raise SystemExit('FAIL: pci_query_device function not found')
body = s[start:end]
if 'pci_write(' in body or 'outl(PCI_CONFIG_DATA' in body:
    raise SystemExit('FAIL: pci_query_device writes PCI configuration space')
for token in ('0x00u', '0x08u', '0x04u', '0x0Cu', '0x10u'):
    if token not in body:
        raise SystemExit('FAIL: required read-only PCI config register missing: ' + token)
print('PASS: generic PCI snapshot uses configuration reads only; no BAR sizing writes')
PYPCI

if [[ -f "$ROOT/kernel/build/kernel.elf" ]] && command -v nm >/dev/null 2>&1; then
    if nm -g --defined-only "$ROOT/kernel/build/kernel.elf" | \
        grep -Eq 'Starlix(Gpu|IntelGpu)'; then
        fail "Intel GPU implementation symbols found in kernel.elf"
    fi
    pass "Built kernel.elf contains no exported Intel/GPU driver implementation symbols"
else
    echo "NOTE: kernel.elf symbol separation check deferred until after kernel build."
fi

if [[ -f "$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/build/intlgpudvs.o" ]] && command -v nm >/dev/null 2>&1; then
    nm -g --defined-only "$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/build/intlgpudvs.o" | \
        grep -Eq '[[:space:]]StarlixGpuFrameDriverEntry$' \
        || fail "Graphics ABI v2 entry missing from external Intel driver object"
    nm -g --defined-only "$ROOT/Rx/Drivers/Gpu/BasicDriver/Intel/build/intlgpudvs.o" | \
        grep -Eq '[[:space:]]StarlixGpuKextEntry$' \
        || fail "Generic PCI-context KEXT entry missing from external Intel driver object"
    pass "Both graphics ABI and external KEXT entry points exist in the separate driver object"
else
    echo "NOTE: external driver object symbol check deferred until after KEXT build."
fi

echo "PASS: Intel external-driver source boundary contract"
