#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PCI_CPP="$ROOT/kernel/src/drivers/pci_resource.cpp"
PCI_POLICY="$ROOT/include/driver/pci_bar_probe_policy.hpp"
PCI_ABI="$ROOT/include/driver/pci_resource_abi.hpp"
KEXT_ABI="$ROOT/include/driver/gpu_kext_abi.hpp"
INTEL="$ROOT/kernel/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp"
MAIN="$ROOT/kernel/src/main.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"

check() { grep -Eq "$1" "$2" && echo "PASS: $3" || { echo "FAIL: $3" >&2; exit 1; }; }

check 'StarlixPciProbeBarResourceV1' "$PCI_CPP" 'kernel resource manager uses the shared BAR sizing policy'
check 'address_space_online_cpu_count\(\) != 1u' "$PCI_CPP" 'PCI configuration sizing is gated to the verified BSP-only environment'
check 'pushfq; popq %0; cli' "$PCI_CPP" 'PCI BAR sizing runs in an interrupt-disabled configuration transaction'
check 'outw\(' "$PCI_CPP" 'PCI Command register is written as 16-bit to avoid Status W1C writes'
check 'decode_disabled' "$PCI_POLICY" 'BAR probe disables I/O and memory decode before sizing'
check 'command_write_attempted' "$PCI_POLICY" 'BAR probe restores the original PCI Command word'
check 'if \(!bars_ok\) return false' "$PCI_POLICY" 'failed BAR restoration keeps I/O and memory decoders disabled'
check 'high_write_attempted' "$PCI_POLICY" '64-bit BAR sizing and restoration handles both dwords'
check 'StarlixPciResourceRegistryClaimV1' "$PCI_CPP" 'generic resource registry enforces exclusive range claims'
check 'StarlixPciResourceRegistryReleaseV1' "$PCI_CPP" 'generic resource ownership tokens can be released'
check 'StarlixPciClaimBarFnV1' "$PCI_ABI" 'versioned generic PCI resource host ABI is declared'
check 'pci_resources' "$KEXT_ABI" 'GPU KEXT receives the generic PCI resource service'
check 'claim_bar' "$INTEL" 'Intel external KEXT claims BAR0 only after device matching'
check 'release_resource' "$INTEL" 'Intel external KEXT releases a failed initialization claim'
check 'pci_claim_bar_resource' "$MAIN" 'kernel visitor exposes generic resource claim service'
check 'pci_resource.cpp' "$BUILD" 'kernel build compiles the generic PCI resource manager'

if grep -Eq '0x8086|0x0152|StarlixIntelGpu' "$PCI_CPP" "$PCI_POLICY" "$PCI_ABI"; then
    echo 'FAIL: generic PCI resource layer contains Intel-specific matching' >&2
    exit 1
fi
echo 'PASS: generic PCI resource layer contains no Intel vendor/device policy'

bash "$ROOT/tests/test_pci_bar_resource_host.sh"
bash "$ROOT/tests/test_intel_gpu_kext_entry.sh"
bash "$ROOT/tests/test_intel_gpu_pci_discovery_contract.sh"
echo 'PCI_RESOURCE_PHASE8_CONTRACT: PASS'
