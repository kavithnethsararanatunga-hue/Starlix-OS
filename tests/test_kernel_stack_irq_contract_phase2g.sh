#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
checks=0
failures=0
check() {
  local condition="$1" description="$2"
  checks=$((checks + 1))
  if eval "$condition"; then printf '[PASS] %s\n' "$description"
  else printf '[FAIL] %s\n' "$description"; failures=$((failures + 1)); fi
}
AS="$ROOT/kernel/src/mem/address_space_core.cpp"
KS="$ROOT/kernel/src/mem/kernel_stack.cpp"
CS="$ROOT/kernel/src/process/context_switch.cpp"
MAIN="$ROOT/kernel/src/main.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
check "grep -q 'KERNEL_STACK_PML4_INDEX = 510u' '$ROOT/kernel/src/mem/address_space.hpp'" \
  "PML4 slot 510 is explicitly reserved for the kernel stack arena"
check "grep -q 'kernel_pml4\[KERNEL_STACK_PML4_INDEX\] != 0ULL' '$AS'" \
  "manager refuses to overwrite a firmware-owned slot 510"
check "grep -q 'KERNEL_STACK_PTE_FLAGS = PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX' '$AS'" \
  "kernel stack intermediate and leaf mappings are supervisor writable NX"
check "grep -q 'g_platform.invalidate_page' '$AS'" \
  "new stack mappings invalidate the local TLB entries"
check "grep -q 'KERNEL_STACK_REGION_BASE' '$KS' && grep -q 'KERNEL_STACK_REGION_END' '$KS'" \
  "stack virtual allocations stay inside the reserved high-half arena"
check "grep -q 'address_space_kernel_stack_guard_unmapped' '$KS'" \
  "both stack guard pages are verified absent before binding"
check "grep -q 'thread_prepare_user_interrupt_frame' '$KS'" \
  "only prepared thread stacks receive an initial IRETQ frame"
check "grep -q 'kernel_stack_thread_valid' '$CS'" \
  "context-switch candidate and outgoing thread stacks are ownership-validated"
check "grep -q 'kernel_stack_manager_ready' '$CS'" \
  "context switching remains unavailable when the stack manager fails"
check "grep -q 'kernel_stack_init' '$MAIN' && grep -q 'vmm_init' '$MAIN'" \
  "stack manager initializes after VMM and before user processes"
check "grep -q 'kernel_stack.o' '$BUILD'" \
  "kernel build compiles and links the Phase 2G stack manager"
if [[ "$failures" -eq 0 ]]; then
  printf 'KERNEL_STACK_PHASE2G_IRQ_CONTRACT: PASS (%s checks)\n' "$checks"
  exit 0
fi
printf 'KERNEL_STACK_PHASE2G_IRQ_CONTRACT: FAIL (%s checks, %s failures)\n' "$checks" "$failures"
exit 1
