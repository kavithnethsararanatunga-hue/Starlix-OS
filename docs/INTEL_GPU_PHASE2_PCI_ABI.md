# STARLIX Intel GPU — Phase 2: Generic PCI Context ABI

## Goal

Keep all Intel-specific matching and graphics-driver code in the external
`kernel/src/drivers/Gpu/Basic/Intel` source tree while defining a versioned,
generic PCI device descriptor that the kernel can later pass to any external
KEXT.

## Implemented

- Added `include/driver/pci_abi.hpp` with a fixed-size, versioned PCI device
  descriptor and six BAR descriptors.
- Added a pure BAR decoder for 32-bit memory, 64-bit memory, I/O, below-1-MiB,
  reserved, unassigned and incomplete 64-bit BAR cases.
- Added generic `pci_query_device()` to the kernel PCI subsystem. It reads the
  selected function's identity, class/revision, command/status, header type and
  BAR values. It does not write PCI configuration space and does not perform
  destructive BAR sizing writes.
- Added `include/driver/gpu_kext_abi.hpp` with a versioned KEXT host context and
  driver API. The context contains a generic PCI descriptor and the existing
  GOP graphics host API.
- Added external `StarlixGpuKextEntry`. It validates the ABI, exact `8086:0152`
  device ID, Display/VGA class, assigned memory BAR0, then exposes the existing
  graphics ABI v2 GOP framebuffer API.
- Updated the SYS package entry symbol and kernel's relocation dry-run target
  to `StarlixGpuKextEntry`.
- Updated SYS relocation tests to validate package-relative image size, section
  count, relocation count and entry bounds instead of depending on the former
  fixed entry offset / relocation count.

## Verification evidence

- GCC and Clang: PCI BAR ABI host tests pass.
- GCC and Clang: external KEXT entry host tests pass, including mismatched ID,
  non-display class, I/O BAR, unassigned BAR and ABI mismatch rejection.
- Existing graphics ABI v1 and v2 host tests pass with GCC and Clang.
- STSYS001 validation and negative tests pass.
- SYS/ELF64 loader host tests pass under Clang and GCC ASan/UBSan; current package
  relocates 5 allocated sections and applies 32 relocations.
- Kernel KEXT dry-run host tests pass under Clang and GCC ASan/UBSan. The entry
  is not called.
- Kernel C++26 build and link pass; `kernel.elf` contains no exported Intel KEXT
  implementation or entry symbols.
- `nm -u` reports no undefined symbols in the external Intel KEXT object.

## Explicitly not implemented

This phase does not execute `StarlixGpuKextEntry` from the kernel. The kernel
still stages the `.kext` payload and performs relocation dry-run only. It does
not allocate executable pages, establish W^X mappings, call the KEXT entry,
reserve the BAR, set PCI command bits, map MMIO, access Intel registers, set up
GGTT/GTT, issue GPU commands, or enable native acceleration. No physical Intel
GPU hardware runtime test was performed.

The new `pci_query_device()` API is a generic kernel-side read-only snapshot
primitive. It is not yet wired into a KEXT driver manager / runtime entry call.
