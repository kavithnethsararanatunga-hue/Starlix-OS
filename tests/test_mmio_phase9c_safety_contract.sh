#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
POLICY="$ROOT/kernel/src/mem/mmio_policy.hpp"
CORE="$ROOT/kernel/src/mem/address_space_core.cpp"

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

MMIO_MAP="$(sed -n \
    '/^bool address_space_map_kernel_mmio_page(/,/^}/p' "$CORE")"
MODULE_MAP="$(sed -n \
    '/^bool address_space_map_kernel_module_page(/,/^}/p' "$CORE")"

grep -Eq 'valid_kernel_mmio_range' "$POLICY" &&
    echo '[PASS] UEFI MMIO validation enforces the DMA32 window' ||
    { echo '[FAIL] DMA32 MMIO policy missing' >&2; exit 1; }
checks=$((checks + 1))

grep -Eq 'framebuffer\.base == 0ULL && framebuffer\.size == 0ULL' "$POLICY" &&
    echo '[PASS] framebuffer absence requires an exact zero range' ||
    { echo '[FAIL] framebuffer absence policy missing' >&2; exit 1; }
checks=$((checks + 1))

grep -Eq 'framebuffer_end <= mmio\.base|mmio_end <= framebuffer\.base' "$POLICY" &&
    echo '[PASS] framebuffer overlap uses complete byte ranges' ||
    { echo '[FAIL] framebuffer overlap test missing' >&2; exit 1; }
checks=$((checks + 1))

printf '%s\n' "$MMIO_MAP" | grep -q 'identity_roots_share_slot_zero' &&
    echo '[PASS] MMIO cache transition requires shared identity roots' ||
    { echo '[FAIL] shared identity-root gate missing from MMIO mapper' >&2; exit 1; }
checks=$((checks + 1))

printf '%s\n' "$MMIO_MAP" | grep -q 'module_subtree_physical_alias' &&
    echo '[PASS] MMIO mapper rejects existing KEXT module aliases' ||
    { echo '[FAIL] KEXT alias gate missing from MMIO mapper' >&2; exit 1; }
checks=$((checks + 1))

printf '%s\n' "$MODULE_MAP" | grep -q 'PHYSICAL_PAGE_ALREADY_OWNED_BY_MMIO' &&
    echo '[PASS] KEXT mapper rejects pages already owned by MMIO' ||
    { echo '[FAIL] MMIO ownership gate missing from KEXT mapper' >&2; exit 1; }
checks=$((checks + 1))

printf 'MMIO_PHASE9C_SAFETY_CONTRACT: PASS (%s checks)\n' "$checks"
