# Starlix OS User Mode Phase 2F — Timer Context Switching

## Scope

Phase 2F connects the Phase 2A–2E process, address-space, scheduler, interrupt-frame and TSS foundations into a single-CPU, timer-driven CPL3 context-switch path. The switch coordinator starts **disarmed**. The normal kernel boot sequence initializes it but does not launch a user process. A future RXFS ECT/session loader must explicitly prepare a process, user image mappings, user stack, caller-owned kernel stack and initial interrupt frame, then call `context_switch_start_first_user()`.

## Timer-switch transaction

A switch is considered only for a PIT IRQ0 that interrupted CPL3. The IRQ assembly saves all 15 general-purpose registers and the normalized interrupt/IRETQ frame, performs `SWAPGS` so C++ runs with the kernel per-CPU GS base, and calls the dispatcher. The coordinator rejects the call unless IF is clear, switching is armed, the current thread/process/CR3/TSS stack match, and the saved frame is fully contained in the owning kernel stack.

For an eligible tick the coordinator:

1. Saves the outgoing user FS base, user GS base (from `IA32_KERNEL_GS_BASE` after entry `SWAPGS`), and FPU/SSE state.
2. Stores the outgoing normalized interrupt frame and requeues the outgoing READY thread.
3. Selects a validated READY thread using the Phase 2D priority / equal-priority round-robin policy.
4. Updates TSS `RSP0`, activates the selected process CR3, restores its FPU/SSE state and FS/GS bases, then updates process/thread states.
5. Returns the selected thread's saved frame pointer. The IRQ assembly restores its GPRs, performs `SWAPGS` for a CPL3 return, and uses `IRETQ`.

Non-user interrupts, non-PIT interrupts, and all interrupts while the coordinator is disarmed keep the current frame. The first user thread is entered through the same GPR restore / `IRETQ` epilogue.

## FPU and extended state policy

The kernel uses eager `FXSAVE64` / `FXRSTOR64` state preservation for x87/MMX/SSE. CPUID FXSR/SSE/SSE2 support is checked. The 512-byte FXSAVE area is 16-byte aligned in each thread object. CR0.EM/TS are cleared, MP/NE enabled, and CR4.OSFXSR/OSXMMEXCPT enabled. CR4.OSXSAVE and CR4.FSGSBASE are cleared: AVX/XSAVE and direct unmediated user FS/GS base writes are not supported in this phase. The kernel build retains `-mgeneral-regs-only` so C++ code does not use SIMD registers outside the explicit FPU backend.

## Deliberate limitations / safety gates

- No first user process is launched during kernel boot. `context_switch_start_first_user()` must be called explicitly by a future loader after all mappings and stack backing are valid.
- This is single-CPU only. There is no SMP run queue, inter-processor TLB shootdown, PCID support, or per-CPU scheduler state.
- A caller must supply a physically backed, supervisor-mapped kernel stack that is mapped in every process address space. Phase 2F validates address/range/alignment/overlap metadata but does not allocate or map kernel-stack pages.
- Only timer IRQ0 from CPL3 can preempt. There is no kernel preemption and no blocking/wakeup integration beyond the scheduler policy APIs.
- EFER.SCE remains clear. The existing SYSCALL/SYSRET interface is not enabled for user mode yet.
- User-originated CPU exceptions remain fatal and are not converted into per-process fault/termination events.
- AVX/XSAVE state is unsupported. FXSAVE covers legacy x87/MMX/SSE state only.
- No actual VMware/physical-machine context-switch test was performed. Host tests validate frame/policy contracts and the kernel/EFI builds compile the privileged path, but they do not execute `SWAPGS`, CR3 writes, `FXSAVE/FXRSTOR`, or `IRETQ` on hardware.
- The Windows-style lock screen, authentication services (`smgrsb.ect`, `stitzn.ect`, `lsass.ect`, `stlgn.ect`) and `Explorer.app` are not implemented by this phase.

## Files introduced or updated

- `kernel/src/cpu/fpu.hpp`, `fpu.cpp`: eager legacy FPU/SSE state backend.
- `kernel/src/process/context_switch.hpp`, `context_switch.cpp`, `context_switch_policy.hpp`: gated first-user entry and IRQ0 context-switch transaction.
- `kernel/src/process/process.hpp`, `process.cpp`: per-thread FPU/FS/GS and saved frame metadata; registered-process validation.
- `kernel/src/mem/address_space.hpp`, `address_space_core.cpp`: registered-root lookup/activation adapter.
- `kernel/src/cpu/idt.cpp`, `idt_asm.S`, `msr.hpp`: CPL3 IRQ dispatch, SWAPGS and alternate IRETQ-frame return.
- `kernel/src/main.cpp`: initialize FPU and disarmed switch coordinator.
- `scripts/build_kernel.sh`: compile and link new units while retaining GPR-only kernel compilation.
- `tests/`: Phase 2F host sanitizer and static IRQ contract checks.
