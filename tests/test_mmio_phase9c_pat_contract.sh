#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSR="$ROOT/kernel/src/cpu/msr.hpp"
POLICY="$ROOT/kernel/src/mem/mmio_policy.hpp"
HEADER="$ROOT/kernel/src/mem/address_space.hpp"
LIVE="$ROOT/kernel/src/mem/address_space.cpp"
TEST="$ROOT/tests/mmio_phase9_policy_selftest.cpp"

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

check 'MSR_IA32_PAT = 0x00000277' "$MSR" \
    'IA32_PAT MSR index is defined as 0x277'
check 'CPUID_FEATURE_PAT = 1u << 16' "$POLICY" \
    'PAT feature is represented by CPUID.1:EDX[16]'
check 'CPUID_FEATURE_MSR = 1u << 5' "$POLICY" \
    'MSR instruction support is represented by CPUID.1:EDX[5]'
check 'select_uc_pat_flags' "$POLICY" \
    'pure policy gates PAT selection on both CPUID features'
check 'address_space_query_uc_pat_flags' "$HEADER" \
    'live read-only UC PAT query is declared'
check 'rdmsr\(starlix::cpu::MSR_IA32_PAT\)' "$LIVE" \
    'live adapter reads IA32_PAT only after CPUID feature gate'
check 'select_uc_pat_flags' "$LIVE" \
    'live adapter uses exact UC PAT selection policy'
check 'runtime PAT policy rejects CPUs without PAT support' "$TEST" \
    'host self-test covers absent PAT feature'

printf 'MMIO_PHASE9C_PAT_CONTRACT: PASS (%s checks)\n' "$checks"
