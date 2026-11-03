#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/kernel/src/process/user_process.cpp"
CTX="$ROOT/kernel/src/process/context_switch.cpp"
SCHED="$ROOT/kernel/src/process/scheduler.cpp"
ASM="$ROOT/kernel/src/cpu/idt_asm.S"
need() { grep -Fq -- "$2" "$1" || { echo "[FAIL] $3" >&2; exit 1; }; echo "[PASS] $3"; }
need "$SRC" 'bool StNtStartUserProcess(const UserProcessHandle* handle)' 'ECT process exposes an explicit first-entry API'
need "$SRC" 'user_process_start_allowed(facts)' 'launch API applies the complete fail-closed policy gate'
need "$SRC" 'scheduler::scheduler_is_admitted(record->thread)' 'launch requires exact scheduler admission'
need "$SRC" 'record->launch_committed = true' 'launch is single-shot once first-entry begins'
need "$SRC" 'context_switch::context_switch_start_first_user(thread)' 'ECT process launches only through the validated IRETQ path'
need "$SRC" 'record->launch_committed = false' 'rejected launch restores the process to abortable preparation state'
need "$SRC" 'record->launch_committed ||' 'committed first-entry process cannot be rolled back as an inactive process'
need "$SCHED" 'bool scheduler_is_admitted(const StThread* thread)' 'scheduler provides pointer-identity admission query'
need "$ROOT/kernel/src/process/context_switch.hpp" 'context_switch_switch_count();' 'context-switch diagnostics are declared for the opt-in smoke harness'
need "$CTX" 'scheduler::scheduler_is_admitted(first_thread)' 'low-level first entry verifies scheduler membership'
need "$CTX" 'scheduler::scheduler_remove(first_thread->tid)' 'first-entry commit removes READY entry before RUNNING state transition'
need "$CTX" 'rollback_or_fail_stop();' 'first-entry failure restores CR3/TSS/FPU/FS/GS or fail-stops safely'
need "$CTX" 'starlix_enter_user_from_frame(' 'validated frame is entered through the common IRETQ restore path'
need "$ASM" 'iretq' 'assembly first-entry path returns to CPL3 with IRETQ'
# Process construction must remain separate from the explicit launch operation.
CREATE_BODY="$(sed -n '/bool StNtCreateUserProcessFromECT/,/bool StNtStartUserProcess/p' "$SRC")"
if grep -Fq 'context_switch_start_first_user' <<<"$CREATE_BODY"; then
  echo '[FAIL] process creation must not launch automatically' >&2; exit 1
fi
echo '[PASS] process creation remains separate from explicit first-user launch'
# Normal kernel configuration must not opt in to the diagnostic Ring-3 test.
need "$ROOT/scripts/build_kernel.sh" 'RING3_SMOKE_TEST="${STARLIX_ENABLE_RING3_SMOKE_TEST:-0}"' 'Ring-3 diagnostic remains opt-in by default'
echo 'USER_PROCESS_START_PHASE2M_CONTRACT: PASS'

need "$CTX" 'address_space_active_root() !=' 'first-entry verifies the committed CR3 before IRETQ'
need "$CTX" 'tss_get_kernel_stack() != first_thread->kernel_stack_top' 'first-entry verifies TSS.RSP0 before IRETQ'
need "$CTX" 'rollback_or_fail_stop();' 'failed rollback halts rather than returning on uncertain machine state'
need "$SRC" '!starlix::process::scheduler::scheduler_is_admitted(record->thread)' 'process abort tolerates a scheduler slot already removed by rejected launch'
