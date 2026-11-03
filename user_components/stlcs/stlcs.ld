/* Fixed-offset, position-independent x86-64 user image layout for ECT v1. */
ENTRY(stlcs_bootstrap)
SECTIONS
{
    . = 0x1000;
    .text : ALIGN(0x1000) { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(0x1000) { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.llvm_addrsig) }
}
