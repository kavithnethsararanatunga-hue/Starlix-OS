/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

.global asm_syscall_handler
.type asm_syscall_handler,@function
.extern StNtSystemCallHandler

asm_syscall_handler:
    /*
     * Entry contract:
     * RAX = syscall number
     * RDI = arg1
     * RSI = arg2
     * RDX = arg3
     * RCX = user return RIP
     * R11 = user return RFLAGS
     *
     * This entry is intended for Ring 3 callers only.
     */

    swapgs

    /* GS now points to StarlixSyscallCpuLocal. */
    movq %rsp, %gs:8
    movq %gs:0, %rsp

    /* SysV C ABI requires DF clear. */
    cld

    /* Preserve SYSRET return state on the dedicated kernel stack. */
    pushq %rcx
    pushq %r11

    /* Remap SYSCALL registers to SysV C function arguments. */
    movq %rdx, %rcx
    movq %rsi, %rdx
    movq %rdi, %rsi
    movq %rax, %rdi

    /* RSP is 16-byte aligned before CALL. */
    call StNtSystemCallHandler

    /* RAX contains the syscall result. */
    popq %r11
    popq %rcx

    /* Restore user stack, then restore user GS state. */
    movq %gs:8, %rsp
    movq $0, %gs:8
    swapgs

    sysretq

.size asm_syscall_handler, .-asm_syscall_handler

/*
 * Ring 3 launch remains explicitly disabled in Phase 1B.
 * A future reviewed transition must establish user address spaces,
 * user GS state, interrupt-entry SWAPGS handling and return validation.
 */
.global jump_to_user_mode
.type jump_to_user_mode,@function
jump_to_user_mode:
    cli
.Lring3_launch_disabled:
    hlt
    jmp .Lring3_launch_disabled

.size jump_to_user_mode, .-jump_to_user_mode

.section .note.GNU-stack,"",@progbits
