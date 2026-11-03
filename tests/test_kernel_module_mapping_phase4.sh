#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AS="$ROOT/kernel/src/mem/address_space.hpp"
CORE="$ROOT/kernel/src/mem/address_space_core.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
checks=0
check() {
  local condition="$1" description="$2"
  checks=$((checks + 1))
  if eval "$condition"; then printf '[PASS] %s\n' "$description"
  else printf '[FAIL] %s\n' "$description"; return 1; fi
}
check "grep -q 'KERNEL_MODULE_PML4_INDEX = 509u' '$AS'" \
  "external-module mappings use dedicated PML4 slot 509"
check "grep -q 'KERNEL_STACK_PML4_INDEX = 510u' '$AS'" \
  "kernel stacks retain their separate PML4 slot 510"
check "grep -q 'kernel_pml4\[KERNEL_MODULE_PML4_INDEX\] != 0ULL' '$CORE'" \
  "manager refuses to overwrite a firmware-owned module PML4 slot"
check "grep -q 'KERNEL_MODULE_TABLE_FLAGS = PAGE_PRESENT | PAGE_WRITABLE' '$CORE'" \
  "module upper-level tables are supervisor-only and leave NX to the PTE"
check "grep -q 'address_space_map_kernel_module_page' '$CORE'" \
  "generic kernel module page mapping API is implemented"
check "grep -q 'address_space_unmap_kernel_module_page' '$CORE'" \
  "generic kernel module unmapping API is implemented"
check "grep -q 'address_space_query_kernel_mapping' '$CORE'" \
  "read-only page-table query reports 4 KiB and huge-page permissions"
check "grep -q 'IDENTITY_ALIAS_WX_CONFLICT' '$CORE'" \
  "mapping refuses a conflicting writable or executable direct identity alias"
check "grep -q 'module_subtree_physical_alias' '$CORE'" \
  "module arena rejects duplicate physical-page aliases"
check "grep -q 'g_platform.invalidate_page(virtual_address)' '$CORE'" \
  "mapping changes invalidate the local TLB entry"
check "grep -q 'address_space_core.o' '$BUILD'" \
  "kernel build links the generic address-space implementation"
printf 'KERNEL_MODULE_MAPPING_PHASE4_CONTRACT: PASS (%s checks)\n' "$checks"
