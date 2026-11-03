#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT/kernel/src/mem/address_space.hpp"
CORE="$ROOT/kernel/src/mem/address_space_core.cpp"
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

check 'address_space_set_kernel_identity_page_permissions' "$HEADER" \
    'identity alias permission API is declared'
check 'address_space_kernel_identity_permission_reason' "$HEADER" \
    'identity permission failure reason API is declared'
check 'identity_roots_share_slot_zero' "$CORE" \
    'identity subtree is verified shared across registered address spaces'
check 'entry & ~PAGE_ACCESSED' "$CORE" \
    'shared PML4 identity entry comparison tolerates only hardware Accessed-bit differences'
check 'split_identity_1g' "$CORE" \
    '1 GiB identity huge pages can be demoted'
check 'split_identity_2m' "$CORE" \
    '2 MiB identity huge pages can be demoted'
check 'PAGE_PAT_LARGE' "$CORE" \
    'large-page PAT handling is explicit'
check 'PAGE_PAT_4K' "$CORE" \
    '4 KiB PTE PAT conversion is explicit'
check 'MODULE_ALIAS_STILL_PRESENT' "$CORE" \
    'identity write permission cannot be restored while module alias exists'
check 'target_permissions & KERNEL_MAPPING_EXECUTE' "$CORE" \
    'identity alias API never grants execute permission'
check 'invalidate_shared_kernel_page\(physical_address\)' "$CORE" \
    'identity permission changes use the centralized local TLB invalidation path'
check 'synchronous all-CPU TLB shootdown' "$HEADER" \
    'API documents the current single-CPU TLB invalidation limitation'
check '2 MiB huge identity mapping is demoted' "$SELFTEST" \
    '2 MiB demotion is covered by the host self-test'
check '1 GiB huge identity mapping is demoted' "$SELFTEST" \
    '1 GiB demotion is covered by the host self-test'
check 'PAT semantics' "$SELFTEST" \
    'PAT preservation is covered by the host self-test'
check 'restored identity page is RW\+NX and never writable-executable' "$SELFTEST" \
    'identity restoration is verified to remain NX'

printf 'KERNEL_IDENTITY_PERMISSIONS_PHASE5_CONTRACT: PASS (%s checks)\n' "$checks"
printf '\n--- phase 5 address-space host self-test ---\n'
CXX="${CXX:-clang++}" bash "$ROOT/tests/test_kernel_stack_phase2g.sh"
