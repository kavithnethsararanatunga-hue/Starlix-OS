# STARLIX OS 1.0.Beta — Intel GPU Track, Phase 6

Copyright (c) 2026 OpenRPX XSoft. All rights reserved.

## Phase 6 result

**Source/build verification: PASS in an isolated Phase 5 reconstruction.**
The patch adds an explicit single-CPU topology gate and centralizes shared
kernel-page invalidation. It does not claim SMP support.

## Why this phase is deliberately single-CPU

The inspected `1.0.Beta.zip` kernel source tree contains no AP startup path,
Local-APIC/x2APIC IPI sender, IOAPIC routing backend, or MADT-based online CPU
manager. Therefore a real synchronous SMP TLB shootdown cannot be provided
honestly from this source baseline. The implementation fails closed rather
than treating a local `INVLPG` as if it invalidated remote CPUs.

## Changes

- `AddressSpacePlatform` now requires an explicit `online_cpu_count`.
- `address_space_manager_init()` rejects a zero/unknown CPU count with
  `CPU_TOPOLOGY_UNAVAILABLE`.
- It rejects `online_cpu_count != 1` with
  `SMP_TLB_SHOOTDOWN_BACKEND_UNAVAILABLE`.
- `kernel/src/mem/address_space.cpp` advertises one CPU because the current
  boot path starts only the bootstrap processor. The comment explicitly says
  AP startup must not be enabled before a synchronous shootdown backend and
  per-CPU active-root tracking are integrated.
- All shared kernel mapping invalidations now pass through
  `invalidate_shared_kernel_page()`. That helper performs the existing local
  `INVLPG` callback and increments an atomic invalidation generation exposed
  by `address_space_tlb_invalidation_generation()`.
- Host tests cover unknown topology, multi-CPU fail-closed behavior, and the
  invalidation generation.
- Existing Phase 2B/2C/2K test platform fixtures now declare their single-CPU
  topology. Phase 2K reclaim expectations account for the persistent module
  and kernel-stack arena page tables introduced by Phase 4.
- Phase 5's contract script now checks the centralized invalidation helper
  and the updated single-CPU documentation.

## Verification performed

On an isolated reconstruction from the Library's `1.0.Beta.zip`, overlaid
with the supplied Phase 1–5 patch sources:

| Verification | Observed result |
|---|---:|
| Phase 6 source contract | 17/17 PASS (Clang and GCC invocations) |
| Main address-space/stack/module self-test | 99/99 PASS (Clang ASan/UBSan) |
| Main address-space/stack/module self-test | 99/99 PASS (GCC ASan/UBSan) |
| Phase 5 identity permission contract | 16/16 PASS |
| Phase 4 module mapping contract | 11/11 PASS |
| Kernel-stack IRQ contract | 11/11 PASS |
| Address-space Phase 2B regression | 49/49 PASS |
| Address-space Phase 2C regression | 64/64 PASS |
| Address-space Phase 2K reclaim regression | 16/16 PASS |
| KEXT temporary staging regression | PASS; 32 relocations; no entry execution |
| SYS package and ELF relocation validation | PASS; no payload execution |
| Intel PCI policy tests | GCC and Clang PASS (host-only) |
| Intel KEXT entry ABI tests | GCC and Clang PASS (host-only) |
| Kernel C++ freestanding build/link | PASS |
| External Intel `intlgpudvs.sys` / `.kext` build | PASS |
| Kernel/KEXT symbol-boundary audit | PASS; no Intel/GPU implementation symbols in `kernel.elf` |
| Clean Phase 5 baseline installer test | PASS |

Observed isolated build artifacts:

- `kernel.elf`: 997,248 bytes; ELF64 x86-64; entry `0x200000`.
- `kernel.bin`: 3,539,504 bytes.
- External `intlgpudvs.sys`: 29,056 bytes; ELF64 relocatable.
- External KEXT object defines `StarlixGpuKextEntry`.
- `kernel.elf` contains no `StarlixGpu*` or `StarlixIntelGpu*` implementation symbols.

## Important limitations

- This is **not** SMP support. The live adapter is BSP-only. No AP startup,
  APIC, IPI, remote invalidation acknowledgements, or per-CPU CR3 tracking is
  implemented. If APs are started later, this manager must remain disabled
  until that backend exists.
- The invalidation generation counts local invalidations; it is not a remote
  shootdown generation and does not prove another CPU flushed its TLB.
- This does not prove a global absence of arbitrary writable aliases. Phase 5
  protects the inspected direct identity alias and the dedicated module arena.
- The Intel KEXT remains separately packaged. Its entry point is **not** loaded
  or executed by this phase. No Intel PCI resource ownership, BAR sizing,
  MMIO mapping, GPU register access, display-engine initialization, or native
  acceleration was exercised.
- Host tests and isolated build results are not physical Intel GPU runtime
  evidence. Apply the patch to the current WSL project and run the included
  commands to verify that working tree.

The original Phase 1 external-source installer had an obsolete exact `main.cpp`
anchor against the Library archive snapshot, so the isolated verification
reconstruction used the package's explicit external-source file overlay and
left the old `main.cpp` block untouched. Phase 6 itself does not modify
`main.cpp`, PCI enumeration, or any Intel-specific kernel source.
