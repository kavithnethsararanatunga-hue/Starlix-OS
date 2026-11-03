#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/kernel/src/main.cpp"
RUNTIME="$ROOT/kernel/src/loader/kext_runtime.hpp"
PCI_H="$ROOT/kernel/src/drivers/pci.hpp"
PCI_CPP="$ROOT/kernel/src/drivers/pci.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
fail(){ echo "FAIL: $1" >&2; exit 1; }
pass(){ echo "PASS: $1"; }

grep -Fq 'using PciDeviceVisitorV1' "$PCI_H" || fail 'generic PCI visitor ABI missing'
grep -Fq 'visitor(device, context)' "$PCI_CPP" || fail 'generic PCI scan does not publish device snapshots'
pass 'generic PCI scanner publishes read-only snapshots to a caller visitor'

grep -Fq 'KextRuntimeLoadExternal(' "$RUNTIME" || fail 'generic runtime loader missing'
grep -Fq 'RelocateSysElf64Image(' "$RUNTIME" || fail 'ELF relocation not wired into runtime loader'
grep -Fq 'address_space_map_kernel_module_page' "$RUNTIME" || fail 'module VA mapping adapter missing'
grep -Fq 'address_space_set_kernel_identity_page_permissions' "$RUNTIME" || fail 'identity alias W^X adapter missing'
grep -Fq 'KextRuntimeRollback(' "$RUNTIME" || fail 'rollback path missing'
grep -Fq 'KextRuntimeUnloadExternal(' "$RUNTIME" || fail 'unload lifecycle missing'
pass 'generic external runtime loader validates, relocates, maps and supports rollback/unload'

grep -Fq 'entry(&host, &driver_api)' "$MAIN" || fail 'external entry invocation missing'
grep -Fq 'device.class_code != 0x03u' "$MAIN" || fail 'kernel visitor not class-filtered'
grep -Fq 'pci_scan_all(' "$MAIN" || fail 'generic PCI visitor path missing'
grep -Fq 'g_external_gpu_graphics_host' "$MAIN" || fail 'graphics host API lifetime missing'
if grep -Eiq '0x8086|0x0152|IsSupportedKextDevice|intel_gpu_pci_policy' "$MAIN" "$PCI_H" "$PCI_CPP" "$RUNTIME"; then
    fail 'Intel PCI identity constants/policy leaked into generic kernel code'
fi
pass 'KEXT entry is invoked only through the generic display-class PCI visitor'

grep -Fq -- '-I$ROOT/include' "$BUILD" || fail 'shared ABI include path not configured for kernel build'
if grep -Eiq 'Gpu/Basic/Intel/intlgpudvs\.cpp|intlgpudvs\.o' "$BUILD"; then
    fail 'Intel driver object leaked into kernel build script'
fi
pass 'kernel build consumes generic ABI headers but never compiles Intel driver source'

grep -Fq 'StarlixGpuKextEntry' "$ROOT/scripts/build_intlgpudvs.sh" || fail 'KEXT build entry metadata missing'
pass 'external KEXT package declares its own runtime entry symbol'

if [[ -f "$ROOT/kernel/build/kernel.elf" ]] && command -v nm >/dev/null; then
    if nm -g --defined-only "$ROOT/kernel/build/kernel.elf" | grep -Eq 'Starlix(Gpu|IntelGpu)'; then
        fail 'kernel ELF exports Intel/GPU driver implementation symbols'
    fi
    pass 'kernel ELF has no exported Intel/GPU driver implementation symbols'
fi

echo 'KEXT_RUNTIME_CONTRACT: PASS'
