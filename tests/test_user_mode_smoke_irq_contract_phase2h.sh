#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PASS=0
FAIL=0
check() {
  local label="$1"; shift
  if "$@"; then printf '[PASS] %s\n' "$label"; PASS=$((PASS+1));
  else printf '[FAIL] %s\n' "$label"; FAIL=$((FAIL+1)); fi
}
contains() { grep -Fq -- "$2" "$1"; }
regex_file() { python3 - "$1" "$2" <<'PY'
import pathlib,re,sys
text=pathlib.Path(sys.argv[1]).read_text()
raise SystemExit(0 if re.search(sys.argv[2], text, re.S) else 1)
PY
}
not_regex_file() { if python3 - "$1" "$2" <<'PY'
import pathlib,re,sys
text=pathlib.Path(sys.argv[1]).read_text()
raise SystemExit(0 if re.search(sys.argv[2], text, re.S) else 1)
PY
then return 1; else return 0; fi; }

H="$ROOT/kernel/src/process/user_mode_smoke_test.hpp"
CPP="$ROOT/kernel/src/process/user_mode_smoke_test.cpp"
PAYLOAD="$ROOT/kernel/src/process/user_probe_payload.hpp"
IDT="$ROOT/kernel/src/cpu/idt.cpp"
ASM="$ROOT/kernel/src/cpu/idt_asm.S"
MAIN="$ROOT/kernel/src/main.cpp"
CS="$ROOT/kernel/src/process/context_switch.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
BOOTBUILD="$ROOT/scripts/build_boot_kernel.sh"

check "Ring-3 smoke test defaults to disabled" contains "$H" '#define STARLIX_ENABLE_RING3_SMOKE_TEST 0'
check "kernel build accepts only explicit opt-in value 1" regex_file "$BUILD" 'STARLIX_ENABLE_RING3_SMOKE_TEST.*:-0'
check "normal main boot does not launch a user thread unless compiled opt-in" regex_file "$MAIN" '#if STARLIX_ENABLE_RING3_SMOKE_TEST.*user_mode_smoke_test::start'
check "smoke test runs only after system boot completion" regex_file "$MAIN" 'SYSTEM BOOT COMPLETE.*user_mode_smoke_test::start'
check "IDT installs DPL3 gate only in opt-in test builds" regex_file "$IDT" '#if STARLIX_ENABLE_RING3_SMOKE_TEST.*0x80.*0xEE'
check "test-only gate targets dedicated probe-exit ISR" contains "$IDT" 'isr_user_probe_exit'
check "probe ISR is compiled only for opt-in builds" regex_file "$ASM" '#if defined\(STARLIX_ENABLE_RING3_SMOKE_TEST\).*isr_user_probe_exit'
check "probe ISR validates CPL3 and performs SWAPGS" regex_file "$ASM" 'testb \$3, 136\(%rsp\).*swapgs'
check "probe ISR calls a noreturn kernel validator" contains "$ASM" 'call starlix_user_probe_exit_dispatch'
check "probe-exit handler never IRETs to arbitrary user code" regex_file "$ASM" 'call starlix_user_probe_exit_dispatch.*cli.*hlt'
check "probe payload has a dedicated non-syscall INT 0x80 instruction" contains "$PAYLOAD" '0xCD, 0x80'
check "payload uses only the defined user address region for counters" contains "$PAYLOAD" 'own_counter < USER_REGION_BASE'
check "payload rejects addresses at the exclusive user boundary" contains "$PAYLOAD" 'peer_counter >= USER_REGION_END'
check "smoke test creates two independent address spaces" regex_file "$CPP" 'address_space_create\(space_a\).*address_space_create\(space_b\)'
check "smoke test maps code and a shared counter page into both roots" regex_file "$CPP" 'map_user_image\(space_a.*map_user_image\(space_b'
check "both processes get independent user stack backing" regex_file "$CPP" 'stack_a_phys = alloc_phys_pages\(USER_STACK_PAGES\).*stack_b_phys = alloc_phys_pages\(USER_STACK_PAGES\)'
check "each thread receives a managed guarded kernel stack" regex_file "$CPP" 'kernel_stack_allocate_default\(kernel_stack_a\).*kernel_stack_allocate_default\(kernel_stack_b\)'
check "each test thread is bound to its stack before scheduling" regex_file "$CPP" 'kernel_stack_bind_thread\(kernel_stack_a, thread_a\).*kernel_stack_bind_thread\(kernel_stack_b, thread_b\)'
check "both READY threads are admitted before first-user entry" regex_file "$CPP" 'scheduler_admit\(thread_a\).*scheduler_admit\(thread_b\).*context_switch_start_first_user\(thread_a\)'
check "test exit validates user selectors and exact instruction return RIP" regex_file "$CPP" 'frame->cs == USER_CODE_SEGMENT.*frame->ss == USER_DATA_SEGMENT.*frame->rip == PROBE_EXIT_RIP'
check "test exit requires at least one successful timer-driven switch" contains "$CPP" 'context_switch_switch_count() >= 1ULL'
check "test exit verifies both user-written shared counters" contains "$CPP" 'counters[0] >= user_probe::COUNTER_THRESHOLD'
check "test exit verifies active CR3 and TSS RSP0 ownership" regex_file "$CPP" 'address_space_active_root\(\).*tss_get_kernel_stack\(\) == current->kernel_stack_top'
check "switch counter is reset at init and incremented only on committed switches" regex_file "$CS" 'g_switch_count, 0ULL.*g_switch_count, 1ULL'
check "test build flag is inherited by the full boot-image build" contains "$BOOTBUILD" './scripts/build_kernel.sh'
check "the test harness does not enable EFER.SCE or generic syscall ABI" not_regex_file "$CPP" 'EFER_SCE|wrmsr\([^\n]*EFER'

printf 'USER_MODE_SMOKE_PHASE2H_IRQ_CONTRACT: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
