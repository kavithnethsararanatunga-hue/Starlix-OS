# Starlix Phase 13C — SYS/ELF64 Relocation Core

## Purpose

Provide a bounded, fail-closed relocation core for the current `STSYS001` x86-64 `ET_REL` Intel GPU driver object. The API prepares an image into caller-owned writable memory and returns a validated entry address and section map. It does **not** allocate kernel pages, change page permissions, register PCI/MMIO resources, invoke the driver entry, or enable GPU acceleration.

## Current live artifact examined

- Package: `Rx/Drivers/Gpu/BasicDriver/Intel/intlgpudvs.sys`
- Package SHA-256: `5d7a983eceba6d5384b34d791361539a2bbe63ecb6c41cb4ba8c0ebfefd6bda6`
- ELF: ELF64, little-endian, x86-64, `ET_REL`
- Runtime alloc executable section: `.ltext`, `0xeff` bytes, alignment 16
- Entry: `StarlixGpuFrameDriverEntry`, section-relative offset `0xe20`
- Relocation section for executable code: `.rela.ltext`, 27 records, all `R_X86_64_64`
- Undefined symbols in the supplied object: none
- Debug sections contain other relocation kinds; those are not loadable sections and are deliberately ignored by the runtime image builder.

## Implementation

`include/driver/sys_elf_loader.hpp`:

- Calls `ValidateSysPackage` first (STSYS001 fields, package CRC and ELF structural validation).
- Loads only `SHF_ALLOC` `SHT_PROGBITS` and `SHT_NOBITS` sections.
- Enforces section count, image size, alignment and W^X constraints.
- Requires one static symbol table and a unique, global, default-visibility `STT_FUNC` entry in an executable loaded section.
- Applies only `R_X86_64_64` `SHT_RELA` relocations whose target section is loadable.
- Rejects undefined/imported, TLS, IFUNC, common and non-loaded symbols; rejects unsupported relocation types, out-of-bounds writes, arithmetic overflow, overlapping buffers and malformed image bases.
- Uses a read-only preflight pass before touching the destination buffer; no partially relocated image is left after a rejected relocation.
- Returns image layout and entry metadata to the caller. The caller must later provide appropriate page mappings and cache/CPU policy before any execution.

## Test

Run from the project root:

```bash
bash tests/test_sys_elf_loader_host.sh
```

The script compiles the host probe with Clang and GCC under ASan/UBSan and uses the existing `intlgpudvs.sys` package. It checks the 27 relocations and adversarial cases. The test only relocates bytes; it never calls the entry point.

## Not completed by Phase 13C

- No connection from the kernel's `StarlixBootModuleKindKextDb` staging loop to this relocator.
- No executable kernel page allocator / W^X page-table mapping or teardown.
- No KEXT init/shutdown lifecycle or import/HostApi dispatch.
- No Intel PCI BAR resource discovery, MMIO UC mapping, GTT/GGTT setup, display modesetting, render/blitter rings, interrupts, or VSync.
- No hardware execution test.

The next stage should integrate a non-executing validation/relocation dry run into the kernel boot flow first, then add executable memory with explicit W^X transitions and lifecycle safeguards. Only after that should an Intel device match and driver initialization be enabled. Hardware tests require matching Intel HD 2500 (`8086:0152`) hardware or verified PCI passthrough. The ordinary VMware guest in the previous logs exposed VMware SVGA II (`15AD:0405`), a different device.
