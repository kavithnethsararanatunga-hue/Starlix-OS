/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

.global asm_load_gdt
.global asm_load_tss

/*
 * void asm_load_gdt(uint64_t pointer)
 * RDI contains the address of the GDT Pointer
 */
asm_load_gdt:
    lgdt (%rdi)            /* Load the Global Descriptor Table */

    mov $0x10, %ax         /* 0x10 is the Kernel Data Segment */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss           /* Update Stack Segment */

    /* Far return to reload Code Segment (CS) with 0x08 */
    pushq $0x08            /* Push Kernel Code Segment */
    leaq .reload_cs(%rip), %rax
    pushq %rax             /* Push return address */
    lretq                  /* Execute far return */
.reload_cs:
    ret

/*
 * void asm_load_tss(uint16_t selector)
 * RDI contains the TSS Segment Selector
 */
asm_load_tss:
    ltr %di                /* Load Task Register */
    ret
