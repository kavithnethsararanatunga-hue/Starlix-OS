# Starlix Kernel Maximum-Debug / No-Size-Optimization Profile

This build profile intentionally keeps Ring-0 code unoptimized and retains
complete DWARF debug information. It does **not** add arbitrary padding to the
runtime image.

Compiler policy:
- `-O0`, `-g3`, `-gdwarf-5`, `-fstandalone-debug`
- `-fno-inline`, `-fno-omit-frame-pointer`, `-fno-optimize-sibling-calls`
- `-fno-lto`, `-fno-merge-all-constants`
- vectorization disabled; function/data sections not split
- `-mgeneral-regs-only` remains in force for kernel code

Link policy:
- retain all linked input sections (`--no-gc-sections`)
- disable identical code folding (`--icf=none`)
- create a build ID and `kernel.map`
- no strip command; `kernel.elf` retains DWARF and symbols

Artifacts:
- `kernel/build/kernel.elf`: full unstripped debug ELF
- `kernel/build/kernel.bin`: loadable sections plus the real BSS extent required
  by the linker symbols; debug-only DWARF sections are not copied into this raw
  runtime payload
- `kernel/build/kernel.map`: linker map
- `kernel/build/kernel.symbols.txt`: sorted demangled symbols
- `kernel/build/kernel.sections.txt`: ELF section inventory

The raw `.bin` and resulting ECT are runtime artifacts. DWARF debug metadata is
kept in `kernel.elf` and sidecars so the boot loader does not try to map debug
sections as executable kernel sections. `kernel.bin` is never enlarged with
synthetic padding; its existing zero-fill extension is exactly the linked BSS
extent.

The existing Phase 2M VMM/Ring-3 failure is not a size-optimization issue. A
larger kernel will not by itself enable NXE, make the inherited PML4 acceptable,
or create the guarded kernel-stack arena. Those need separate runtime fixes.
