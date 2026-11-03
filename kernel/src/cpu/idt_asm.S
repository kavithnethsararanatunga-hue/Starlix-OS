/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

.global asm_load_idt
.global isr_default_handler
.global starlix_irq_dispatch

.extern starlix_exception_dispatch
.extern starlix_exception_active

.global isr_irq0
.global isr_irq1
.global isr_irq2
.global isr_irq3
.global isr_irq4
.global isr_irq5
.global isr_irq6
.global isr_irq7
.global isr_irq8
.global isr_irq9
.global isr_irq10
.global isr_irq11
.global isr_irq12
.global isr_irq13
.global isr_irq14
.global isr_irq15

.section .text

.type asm_load_idt,@function
asm_load_idt:
    lidt (%rdi)
    ret

/* Unhandled non-exception vectors remain fail-stop. */
.type isr_default_handler,@function
isr_default_handler:
    cli
.Lunhandled_halt:
    hlt
    jmp .Lunhandled_halt

/*
 * CPU exception entry:
 *
 * All vectors 0..31 use an IST stack.
 * CPU pushes SS, RSP, RFLAGS, CS and RIP.
 * Error-code exceptions additionally receive a CPU error code.
 *
 * Both stub types normalize the frame to:
 * vector, error_code, RIP, CS, RFLAGS, RSP, SS
 *
 * GPRs are saved before entering the fatal C++ exception dispatcher.
 * This exception path does not return or recover user faults; user-originated
 * exceptions remain unsupported until process-fault handling is implemented.
 */

.macro ISR_NOERR vector
.global isr_exception_\vector
.type isr_exception_\vector,@function
isr_exception_\vector:
    pushq $0
    pushq $\vector
    jmp isr_exception_common
.endm

.macro ISR_ERROR vector
.global isr_exception_\vector
.type isr_exception_\vector,@function
isr_exception_\vector:
    /* CPU already pushed the error code. */
    pushq $\vector
    jmp isr_exception_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERROR 8
ISR_NOERR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERROR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERROR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERROR 29
ISR_ERROR 30
ISR_NOERR 31

.type isr_exception_common,@function
isr_exception_common:
    /*
     * Claim the fatal-exception guard before using the IST stack.
     * BTS uses no general-purpose register; interrupted RFLAGS
     * are already preserved in the CPU exception frame.
     */
    lock btsl $0, starlix_exception_active(%rip)
    jc .Lnested_exception_halt

    cli
    cld

    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /*
     * The normalized frame is 16-byte aligned:
     * 176 bytes from an aligned IST top.
     */
    movq %rsp, %r12
    movq %rsp, %rdi
    andq $-16, %rsp
    call starlix_exception_dispatch

    /* Defensive fail-stop if the noreturn dispatcher returns. */
    movq %r12, %rsp

.Lnested_exception_halt:
    cli
.Lexception_halt_loop:
    hlt
    jmp .Lexception_halt_loop

/* ---------------- Existing PIC IRQ stubs ---------------- */
/*
 * User-originated IRQs SWAPGS to the kernel per-CPU area before C++ and
 * SWAPGS back immediately before an IRETQ to CPL3. The context-switch
 * coordinator remains disarmed until a validated first user thread is launched.
 */

.type isr_irq0,@function
isr_irq0:
    pushq $32
    jmp isr_irq_common

.type isr_irq1,@function
isr_irq1:
    pushq $33
    jmp isr_irq_common

.type isr_irq2,@function
isr_irq2:
    pushq $34
    jmp isr_irq_common

.type isr_irq3,@function
isr_irq3:
    pushq $35
    jmp isr_irq_common

.type isr_irq4,@function
isr_irq4:
    pushq $36
    jmp isr_irq_common

.type isr_irq5,@function
isr_irq5:
    pushq $37
    jmp isr_irq_common

.type isr_irq6,@function
isr_irq6:
    pushq $38
    jmp isr_irq_common

.type isr_irq7,@function
isr_irq7:
    pushq $39
    jmp isr_irq_common

.type isr_irq8,@function
isr_irq8:
    pushq $40
    jmp isr_irq_common

.type isr_irq9,@function
isr_irq9:
    pushq $41
    jmp isr_irq_common

.type isr_irq10,@function
isr_irq10:
    pushq $42
    jmp isr_irq_common

.type isr_irq11,@function
isr_irq11:
    pushq $43
    jmp isr_irq_common

.type isr_irq12,@function
isr_irq12:
    pushq $44
    jmp isr_irq_common

.type isr_irq13,@function
isr_irq13:
    pushq $45
    jmp isr_irq_common

.type isr_irq14,@function
isr_irq14:
    pushq $46
    jmp isr_irq_common

.type isr_irq15,@function
isr_irq15:
    pushq $47
    jmp isr_irq_common

.type isr_irq_common,@function
isr_irq_common:
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* User GS is exchanged for the kernel per-CPU GS before C++ dispatch. */
    testb $3, 136(%rsp)
    jz .Lirq_kernel_gs_active
    swapgs
.Lirq_kernel_gs_active:
    movq 120(%rsp), %rdi
    movq %rsp, %rsi
    movq %rsp, %r12
    andq $-16, %rsp
    call starlix_irq_dispatch

    /* A non-zero return value selects another validated interrupt frame. */
    testq %rax, %rax
    jz .Lirq_restore_current
    movq %rax, %rsp
    jmp .Lirq_restore_registers
.Lirq_restore_current:
    movq %r12, %rsp
.Lirq_restore_registers:
    /* Restore the target thread's user GS state immediately before IRETQ. */
    testb $3, 136(%rsp)
    jz .Lirq_no_return_swapgs
    swapgs
.Lirq_no_return_swapgs:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $8, %rsp
    iretq


/* Restricted DPL3 Lock Screen HostApi gate (INT 0x81). This is not a general
   syscall ABI: the C dispatcher accepts only the bound stlcs thread, validates
   its CR3/TSS/IRET frame and exposes a fixed set of bounded operations. */
.global isr_user_service
.type isr_user_service,@function
.extern starlix_user_service_dispatch
isr_user_service:
    pushq $129
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    testb $3, 136(%rsp)
    jz .Luser_service_no_swapgs_in
    swapgs
.Luser_service_no_swapgs_in:
    movq %rsp, %rdi
    movq %rsp, %r12
    andq $-16, %rsp
    call starlix_user_service_dispatch
    movq %r12, %rsp
    testb $3, 136(%rsp)
    jz .Luser_service_no_swapgs_out
    swapgs
.Luser_service_no_swapgs_out:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $8, %rsp
    iretq
.size isr_user_service, .-isr_user_service

/* Enter the first Ring-3 thread from its prepared interrupt-return frame.
   The frame layout is identical to the alternate-frame IRQ restore path. */
.global starlix_enter_user_from_frame
.type starlix_enter_user_from_frame,@function
starlix_enter_user_from_frame:
    cli
    movq %rdi, %rsp
    jmp .Lirq_restore_registers
.size starlix_enter_user_from_frame, .-starlix_enter_user_from_frame

#if defined(STARLIX_ENABLE_RING3_SMOKE_TEST) && STARLIX_ENABLE_RING3_SMOKE_TEST
/*
 * Test-only DPL3 INT 0x80 exit gate. A CPL3 invocation switches to TSS.RSP0;
 * the frame is normalized exactly like a PIC IRQ and SWAPGS enters kernel GS.
 * The C dispatcher is noreturn and validates the smoke-test invariants before
 * halting. This is not a general syscall ABI and is not installed normally.
 */
.global isr_user_probe_exit
.type isr_user_probe_exit,@function
.extern starlix_user_probe_exit_dispatch
isr_user_probe_exit:
    pushq $128
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    testb $3, 136(%rsp)
    jz .Luser_probe_exit_bad_cpl
    swapgs
    movq %rsp, %rdi
    andq $-16, %rsp
    call starlix_user_probe_exit_dispatch
.Luser_probe_exit_bad_cpl:
    cli
.Luser_probe_exit_halt:
    hlt
    jmp .Luser_probe_exit_halt
.size isr_user_probe_exit, .-isr_user_probe_exit
#endif

.section .note.GNU-stack,"",@progbits
