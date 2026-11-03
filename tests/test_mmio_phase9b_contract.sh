#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT/kernel/src/mem/address_space.hpp"
CORE="$ROOT/kernel/src/mem/address_space_core.cpp"
LIVE="$ROOT/kernel/src/mem/address_space.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
SELFTEST="$ROOT/tests/kernel_stack_phase2g_selftest.cpp"

checks=0
check() {
    local pattern="$1" file="$2" description="$3"
    checks=$((checks + 1))
    if grep -Eq "$pattern" "$file"; then
        printf '[PASS] %s\n' "$description"
    else
        printf '[FAIL] %s\n' "$description" >&2
        return 1
    fi
}

check 'KERNEL_MMIO_PML4_INDEX = 508u' "$HEADER" \
    'generic MMIO arena reserves PML4 slot 508'
check 'KERNEL_MODULE_PML4_INDEX = 509u' "$HEADER" \
    'external KEXT arena remains in PML4 slot 509'
check 'KERNEL_STACK_PML4_INDEX = 510u' "$HEADER" \
    'kernel stack arena remains in PML4 slot 510'
check 'address_space_map_kernel_mmio_page' "$HEADER" \
    'generic MMIO page mapping API is declared'
check 'address_space_unmap_kernel_mmio_page' "$CORE" \
    'exact MMIO page unmapping API is implemented'
check 'identity_old_cache_flags' "$CORE" \
    'original identity cache attributes are retained for restoration'
check 'flush_caches\(\)' "$CORE" \
    'cache-type transitions flush CPU caches'
check 'flush_caches_adapter' "$LIVE" \
    'live x86 adapter implements the cache flush service'
check 'wbinvd' "$LIVE" \
    'live cache flush uses WBINVD'
check 'KERNEL_MMIO_PML4_INDEX' "$CORE" \
    'address-space initialization reserves and installs slot 508'
check 'same physical MMIO page cannot receive a second virtual alias' "$SELFTEST" \
    'host tests reject duplicate MMIO physical aliases'
check 'identity alias restores its original WB cache attributes' "$SELFTEST" \
    'host tests verify identity cache restoration'
check 'WBINVD backend flushes before UC transition and identity restore' "$SELFTEST" \
    'host tests verify cache flush ordering'

printf 'KERNEL_MMIO_PHASE9B_CONTRACT: PASS (%s checks)\n' "$checks"
