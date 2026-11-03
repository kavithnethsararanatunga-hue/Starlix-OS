/*
 * Starlix kernel bootstrap entry.
 * Called by rxload.efi using the System V x86-64 ABI.
 */

.section .text
.global StarlixKernelEntry
.type StarlixKernelEntry,@function
.extern KernelMain

StarlixKernelEntry:
    cli
    cld

    /* RDI contains StarlixBootInfo*. */
    leaq __starlix_boot_stack_top(%rip), %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp

    /* SysV ABI: RSP is 16-byte aligned before CALL. */
    call KernelMain

.Lkernel_main_returned:
    cli
    hlt
    jmp .Lkernel_main_returned

.size StarlixKernelEntry, .-StarlixKernelEntry

/* Dedicated 64 KiB kernel bootstrap stack. */
.section .bss.stack,"aw",@nobits
.balign 4096

.global __starlix_boot_stack_bottom
__starlix_boot_stack_bottom:
.skip 65536

.balign 16
.global __starlix_boot_stack_top
__starlix_boot_stack_top:

.section .note.GNU-stack,"",@progbits
