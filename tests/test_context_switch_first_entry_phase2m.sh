#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
ctx = (root / "kernel/src/process/context_switch.cpp").read_text()
asm = (root / "kernel/src/cpu/idt_asm.S").read_text()
sched = (root / "kernel/src/process/scheduler.cpp").read_text()
checks = []
def check(ok, name):
    checks.append((bool(ok), name))

start = ctx[ctx.find("bool context_switch_start_first_user("):ctx.find("uint64_t context_switch_on_timer(")]
check("scheduler::scheduler_is_admitted(first_thread)" in start,
      "first entry requires the exact registered READY thread in the scheduler")
check("old_root != starlix::mem::address_space_kernel_root()" in start,
      "first entry begins only from the active kernel address space")
check("valid_thread_switch_state(*thread)" in ctx,
      "saved user IRETQ frame and kernel stack are validated before switching")
check("fpu_state_save(rollback_fpu)" in start,
      "kernel FPU/SSE state is saved before preparing user state")

ordered = [
    "tss_set_kernel_stack_checked(\n            first_thread->kernel_stack_top)",
    "address_space_activate_root(\n            first_process->page_directory_cr3)",
    "fpu_state_restore(first_thread->fpu_state)",
    "address_space_active_root() !=\n            first_process->page_directory_cr3",
    "wrmsr(starlix::cpu::MSR_GS_BASE, kernel_gs)",
    "scheduler::scheduler_remove(first_thread->tid)",
    "process_set_state(first_process, PROCESS_STATE_RUNNING)",
    "thread_set_state(first_thread, THREAD_STATE_RUNNING)",
    "__atomic_store_n(&g_current_thread, first_thread",
    "__atomic_store_n(&g_armed, 1u",
    "starlix_enter_user_from_frame("
]
positions = [start.find(x) for x in ordered]
check(all(x >= 0 for x in positions) and positions == sorted(positions),
      "TSS, CR3, FPU, selectors, scheduler removal, state commit and IRETQ are ordered")
check("rollback_or_fail_stop" in start and "for (;;) asm volatile(\"hlt\"" in start,
      "failed rollback cannot return with an uncertain privileged CPU state")
check("scheduler::scheduler_admit(first_thread)" in start,
      "failed lifecycle commit attempts to restore scheduler admission")
check("return user_process_start_allowed(facts)" not in start,
      "low-level context switch does not bypass the process start policy")
check("movq %rdi, %rsp" in asm and "jmp .Lirq_restore_registers" in asm and
      ".Lirq_restore_registers:" in asm and "iretq" in asm,
      "first entry restores the same normalized interrupt frame with IRETQ")
check("swapgs" in asm[asm.find(".Lirq_restore_registers:"):asm.find("#if defined(STARLIX_ENABLE_RING3_SMOKE_TEST")],
      "common return epilogue performs SWAPGS for a CPL3 target")
check("bool scheduler_is_admitted(const StThread* thread)" in sched,
      "scheduler exposes pointer-identity admission query")

for ok, name in checks:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
print(f"CONTEXT_SWITCH_FIRST_ENTRY_PHASE2M: {'PASS' if all(x[0] for x in checks) else 'FAIL'} ({sum(x[0] for x in checks)}/{len(checks)} checks)")
if not all(x[0] for x in checks):
    raise SystemExit(1)
PY
