#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="$ROOT/kernel/src/mem/address_space.hpp"
CORE="$ROOT/kernel/src/mem/address_space_core.cpp"
ADAPTER="$ROOT/kernel/src/mem/address_space.cpp"
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
check 'uint32_t online_cpu_count;' "$HEADER" 'address-space platform carries explicit online CPU count'
check 'address_space_online_cpu_count\(\)' "$HEADER" 'accepted CPU topology is queryable'
check 'address_space_tlb_invalidation_generation\(\)' "$HEADER" 'shared-kernel invalidation generation is queryable'
check 'platform.online_cpu_count == 0u' "$CORE" 'unknown CPU topology is rejected'
check 'platform.online_cpu_count != 1u' "$CORE" 'multi-CPU address-space startup fails closed without SMP shootdown support'
check 'SMP_TLB_SHOOTDOWN_BACKEND_UNAVAILABLE' "$CORE" 'SMP refusal has an explicit diagnostic'
check 'platform.online_cpu_count = 1u;' "$ADAPTER" 'live adapter explicitly advertises current BSP-only boot topology'
check 'invalidate_shared_kernel_page\(uint64_t virtual_address\)' "$CORE" 'shared kernel invalidations use one central path'
check '__atomic_add_fetch\(&g_tlb_invalidation_generation' "$CORE" 'invalidation generation advances after local INVLPG'
check 'unknown online CPU count' "$SELFTEST" 'host self-test covers missing CPU topology'
check 'multiple CPUs lack an SMP shootdown backend' "$SELFTEST" 'host self-test covers SMP fail-closed behavior'
check 'local invalidation generation' "$SELFTEST" 'host self-test verifies invalidation generation'
check 'online_cpu_count = 1u;' "$ROOT/tests/address_space_phase2b_selftest.cpp" 'Phase 2B host fixture declares the single-CPU topology'
check 'online_cpu_count = 1u;' "$ROOT/tests/address_space_phase2c_selftest.cpp" 'Phase 2C host fixture declares the single-CPU topology'
check 'online_cpu_count = 1u;' "$ROOT/tests/address_space_phase2k_reclaim_selftest.cpp" 'Phase 2K reclaim fixture declares the single-CPU topology'
check 'module/stack arenas remain' "$ROOT/tests/address_space_phase2k_reclaim_selftest.cpp" 'Phase 2K reclaim accounting includes persistent module and stack page tables'
# Exactly one direct call to the low-level callback must remain: inside the central helper.
count="$(grep -c 'g_platform\.invalidate_page(' "$CORE" || true)"
[[ "$count" == "1" ]] || { echo '[FAIL] low-level INVLPG callback is bypassed outside central helper' >&2; exit 1; }
printf '[PASS] all shared kernel invalidation sites are centralized\n'
checks=$((checks + 1))
printf 'KERNEL_SMP_TLB_PHASE6_CONTRACT: PASS (%s checks)\n' "$checks"
printf '\n--- GCC/Clang address-space host self-test ---\n'
CXX="${CXX:-clang++}" bash "$ROOT/tests/test_kernel_stack_phase2g.sh"
