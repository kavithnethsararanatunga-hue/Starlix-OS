# Starlix OS User Mode — Phase 2B

## Scope

Phase 2B adds a four-level x86-64 page-table core for independent user address
spaces. It is deliberately a memory-management milestone, not a Ring 3 launch
milestone.

## Added source

- `kernel/src/mem/address_space.hpp`
- `kernel/src/mem/address_space_core.cpp`
- `kernel/src/mem/address_space.cpp`
- `tests/address_space_phase2b_selftest.cpp`
- `tests/test_address_space_phase2b.sh`

## Design

1. The hardware adapter reads CR0, CR3, CR4, EFER and the CPU physical-address
   width. It does not write CR0/CR3/CR4/EFER.
2. Initialization accepts only four-level paging, PAE, paging enabled,
   supervisor write protection enabled, long mode active and NX enabled.
3. The kernel PML4 physical address must be identity-accessible below 4 GiB,
   its low identity entry must exist, and PML4 slot 1 must be entirely empty.
   If these assumptions are not true, the feature remains unavailable.
4. Each new address space gets a private PML4 page copied from the current
   kernel root. The U/S bit is cleared in every inherited root entry, so those
   mappings remain supervisor-only in the clone. The source kernel PML4 is not
   modified.
5. PML4 slot 1 is private to the address space and receives newly allocated
   PDPT/PD/PT pages on demand. Only 4 KiB user mappings within the reserved
   `0x0000008000000000` to `0x0000010000000000` range are accepted.
6. User leaf mappings enforce W^X. Non-executable leaves carry NX; writable
   executable mappings are rejected. Translation rejects huge-page paths,
   corrupted NX parents, invalid frames and writable/executable leaves.
7. Address-space and page-table pages are boot-lifetime allocations. There is
   no free/reclamation API in this phase.

## Build integration

`scripts/build_kernel.sh` now compiles and links `address_space_core.cpp` and
`address_space.cpp`. `vmm_init()` performs a read-only inherited-paging check
and reports whether the address-space manager is available.

## Validation

Run:

```bash
bash tests/test_address_space_phase2b.sh
bash scripts/build_kernel.sh
bash scripts/build.sh
bash tests/test_user_mode_phase2a.sh
bash tests/test_rxfs_native.sh
bash tests/test_phase1c2_exception_static.sh
bash tests/test_phys_alloc_math.sh
```

The Phase 2B host test uses a synthetic page-table backend with ASan/UBSan; it
checks cloning, supervisor-only inherited mappings, per-process isolation,
4 KiB mapping/translation/unmapping, W^X/NX, malformed table rejection, and
capacity limits.

## Explicit limitations

- No CR3 switch or TLB activation is performed.
- `jump_to_user_mode` remains disabled; EFER.SCE remains clear.
- No scheduler/context switch, interrupt-return-to-user path or syscall entry
  activation is implemented.
- No physical-page or page-table reclamation exists.
- The process manager is not yet automatically creating/owning these
  `AddressSpace` objects; the later process/loader integration milestone must
  connect each process CR3 to its address-space record.
- A real VMware boot must inspect the inherited UEFI CR3/PML4 layout. In
  particular, PML4 slot 1 must be empty. Host tests and compilation do not
  prove the live UEFI page-table assumptions.

## Safety rule

Do not enable Ring 3 or load one of these CR3 roots until live VMware validation
has confirmed the inherited mapping assumptions and a separate reviewed
kernel-entry/interrupt-return design is complete.
