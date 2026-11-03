# STARLIX OS 1.0.Beta — Intel GPU Phase 8

## Generic PCI BAR resource sizing and exclusive ownership

Copyright (c) 2026 OpenRPX XSoft. All rights reserved.

### Scope

This phase adds a generic PCI resource service used by external drivers. The kernel does not contain Intel vendor/device matching policy. The Intel-specific KEXT checks its own PCI identity (`8086:0152`) first, then requests BAR0 through the generic resource ABI.

The resource service:

- validates the caller's PCI snapshot against the current BDF, vendor/device IDs, header type, and BAR value before probing;
- supports 32-bit and 64-bit memory BAR sizing;
- temporarily disables PCI I/O and memory decoding while probing;
- writes the PCI Command register using a 16-bit transaction only, avoiding writes to PCI Status W1C bits;
- restores BAR dwords and verifies them before restoring the original Command word;
- leaves I/O and memory decoding disabled if BAR restoration cannot be verified;
- rejects unsupported I/O, below-1MiB, reserved, unassigned, and continuation BAR entries;
- provides a bounded exclusive resource registry with opaque ownership tokens and overlap rejection;
- gates configuration transactions to the verified BSP-only topology and disables local interrupts during the transaction.

No BAR address is reassigned. No MMIO mapping or GPU register access is performed in Phase 8.

### Driver boundary

`kernel/src/drivers/pci_resource.cpp` is generic kernel infrastructure. The Intel policy remains in:

`kernel/src/drivers/Gpu/Basic/Intel/intlgpudvs.cpp`

The KEXT obtains a BAR0 resource only after matching its own PCI identity. The resource descriptor is passed back to the KEXT through `gpu_kext_abi.hpp`. The Intel implementation remains a separate `intlgpudvs.sys` / `intlgpudvs.kext` payload and is not linked into `kernel.elf`.

### PCI configuration safety

Sizing a BAR requires a temporary all-ones write while decoding is disabled. This is performed only after the external driver asks for the resource. The service snapshots the original BAR and command state, performs the probe, restores the original BAR value(s), verifies read-back, and only then restores the original command word. If BAR restore/read-back fails, the function fails closed and does not re-enable I/O or memory decoding.

This is a single-CPU configuration transaction, not an SMP-safe PCI lock. Phase 6 rejects multi-CPU address-space startup until a synchronous remote TLB shootdown backend exists; Phase 8 retains that BSP-only constraint. The ownership registry only arbitrates ranges claimed through this PCI service; it is not yet a system-wide UEFI/ACPI resource broker and does not reserve BAR apertures in the physical RAM allocator. Phase 9 must cross-check the full aperture against platform memory/resource descriptors before mapping MMIO.

### Verification performed in isolated Phase 7 baseline

- GCC ASan/UBSan PCI BAR host tests: pass.
- Clang ASan/UBSan PCI BAR host tests: pass.
- External KEXT entry tests with GCC and Clang: pass.
- Generic PCI resource source/contract tests: pass.
- External KEXT runtime host test with GCC and Clang: pass.
- SYS package and ELF relocation regression tests: pass.
- Phase 2B/2C/2K, Phase 4/5/6 memory regressions: pass.
- Kernel build and external Intel KEXT build: pass.
- Kernel symbol audit: no `StarlixGpu*` or `StarlixIntelGpu*` implementation symbols in `kernel.elf`.
- External KEXT object undefined-symbol audit: empty.

The measured build sizes and SHA-256 values are recorded in `proof/phase8/` in the patch archive.

### Not yet proven

No physical PCI hardware was accessed during host tests. The Phase 8 host tests use a mock PCI configuration backend. BAR0 ownership, successful hardware BAR probing, MMIO mapping, Intel Gen7 register access, KEXT execution in QEMU/real kernel runtime, and native Intel acceleration remain unproven. VMware may present a VMware SVGA adapter instead of the target Intel `8086:0152` device.
