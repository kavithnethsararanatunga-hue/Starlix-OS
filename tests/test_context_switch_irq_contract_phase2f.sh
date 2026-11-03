#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
checks = []

def require(condition, message):
    checks.append((bool(condition), message))

idt = (root / "kernel/src/cpu/idt.cpp").read_text()
asm = (root / "kernel/src/cpu/idt_asm.S").read_text()
ctx = (root / "kernel/src/process/context_switch.cpp").read_text()
fpu = (root / "kernel/src/cpu/fpu.cpp").read_text()
syscall = (root / "kernel/src/cpu/syscall.cpp").read_text()
process = (root / "kernel/src/process/process.hpp").read_text()
main = (root / "kernel/src/main.cpp").read_text()
build = (root / "scripts/build_kernel.sh").read_text()

require('#include "../process/context_switch.hpp"' in idt,
        "IDT dispatcher includes the context-switch coordinator")
require('if (irq == 0u && frame && (frame->cs & 3ULL) == 3ULL)' in idt,
        "only timer IRQ0 from CPL3 requests a context switch")
require('context_switch_on_timer(frame)' in idt,
        "IRQ dispatcher returns the alternate frame selected by the coordinator")
require('if (!interrupts_are_disabled() || !context_switch_armed()' in ctx,
        "timer switch requires IF clear and the explicit armed state")
require('return 0ULL;' in idt[idt.find('starlix_irq_dispatch'):],
        "all non-user and non-timer IRQ paths retain the current frame")
require('starlix_enter_user_from_frame' in asm and
        'movq %rdi, %rsp' in asm and 'jmp .Lirq_restore_registers' in asm,
        "first-user entry uses the same audited GPR/IRETQ restore epilogue")
require('testb $3, 136(%rsp)' in asm and 'swapgs' in asm,
        "user IRQ entry and user IRETQ return bracket C++ with SWAPGS")
require('user faults' in asm and 'does not return or recover' in asm,
        "unhandled user exceptions remain explicitly fatal until process-fault support")
require('fpu_state_save(current->fpu_state)' in ctx,
        "timer switch saves the outgoing thread FPU/SSE state")
require('fpu_state_restore(next->fpu_state)' in ctx,
        "timer switch restores the incoming thread FPU/SSE state")
require('MSR_FS_BASE' in ctx and 'MSR_KERNEL_GS_BASE' in ctx and
        'current->fs_base = outgoing_fs' in ctx and
        'current->user_gs_base = outgoing_gs' in ctx,
        "timer switch saves and restores each thread FS/GS base")
require('validate_user_base(*out_process, thread->fs_base)' in ctx and
        'validate_user_base(*out_process, thread->user_gs_base)' in ctx,
        "thread FS/GS bases must resolve to readable user mappings")
require('fs_base' in process and 'user_gs_base' in process and
        'FpuState fpu_state' in process,
        "thread object owns independent FPU and FS/GS state")
require('address_space_activate_root(' in ctx,
        "context switch changes to the incoming registered CR3 root")
require('tss_set_kernel_stack_checked(' in ctx,
        "context switch updates TSS.RSP0 for the incoming kernel stack")
require('scheduler::scheduler_pick_next()' in ctx and
        'scheduler::scheduler_admit(current)' in ctx,
        "timer switch connects READY admission and scheduler selection")
require('__atomic_store_n(&g_armed, 0u' in ctx,
        "context-switch coordinator initializes disarmed")
require('context_switch_start_first_user' in ctx,
        "a validated first-user entry is required before arming preemption")
require('(CR0_EM | CR0_TS)' in fpu and 'CR4_OSFXSR' in fpu and
        'CR4_OSXMMEXCPT' in fpu and 'CR4_OSXSAVE' in fpu,
        "FPU initialization configures x87/SSE and keeps XSAVE/AVX disabled")
require('CR4_FSGSBASE' in fpu and 'cr4 &= ~(CR4_OSXSAVE | CR4_FSGSBASE)' in fpu,
        "unmediated user FSGSBASE instructions remain disabled")
require('EFER_SCE' in syscall and 'final_efer & ~EFER_SCE' in syscall,
        "SYSCALL/SYSRET remains disabled until its safe user ABI is implemented")
require('fxsave64' in fpu and 'fxrstor64' in fpu,
        "FPU backend implements eager FXSAVE/FXRSTOR state preservation")
require('CPUID_FXSR | CPUID_SSE | CPUID_SSE2' in fpu,
        "FPU backend verifies required CPU instruction support")
require(main.find('idt_init()') < main.find('scheduler_init()') <
        main.find('fpu_init()') < main.find('context_switch_init()') and
        'pushfq; popq %0; cli' in main,
        "FPU state is initialized after scheduler with interrupts disabled before context switching")
require(main.find('scheduler_init()') < main.find('context_switch_init()'),
        "context-switch coordinator initializes after process scheduler")
require('context_switch_start_first_user(' not in main,
        "boot does not launch Ring 3 until the future ECT/session loader calls it")
require('-mgeneral-regs-only' in build and 'cpu/fpu.cpp' in build and
        'process/context_switch.cpp' in build,
        "kernel build keeps C++ GPR-only and links FPU/context-switch units")

for ok, label in checks:
    print(f"[{'PASS' if ok else 'FAIL'}] {label}")
print(f"CONTEXT_SWITCH_IRQ_CONTRACT_PHASE2F: {'PASS' if all(x[0] for x in checks) else 'FAIL'} ({sum(x[0] for x in checks)}/{len(checks)} checks)")
if not all(x[0] for x in checks):
    raise SystemExit(1)
PY
