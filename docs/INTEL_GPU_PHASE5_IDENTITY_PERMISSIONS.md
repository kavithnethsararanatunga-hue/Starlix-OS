# STARLIX Intel GPU — Phase 5: Identity Alias Permissions

Copyright (c) 2026 OpenRPX XSoft. All rights reserved.

## Scope

Phase 5 adds a generic kernel API to change the permissions of one managed
4 KiB page in the low physical identity mapping. The API keeps the identity
alias NX and permits only RW+NX or RO+NX as the resulting identity mapping.
A separately mapped module page can then be RO+X without a writable identity
alias for the same physical page.

The implementation can demote an existing 2 MiB or 1 GiB identity huge-page
mapping to 4 KiB leaves. The large-page PAT bit is converted to the 4 KiB PTE
PAT bit when required. Other cache, accessed, dirty, global, and software
flags are retained where applicable.

## API

`address_space_set_kernel_identity_page_permissions(physical_address,
expected_permissions, target_permissions)`

`expected_permissions` and `target_permissions` use the generic
`KERNEL_MAPPING_WRITE` and `KERNEL_MAPPING_EXECUTE` bits. The identity alias
API never grants execute permission; the target may only be read-only+NX or
read-write+NX. The expected-permission token prevents a stale permission
update. An identity page cannot be made writable while the same physical page
is still mapped in the external-module arena.

## Intended external-module mapping sequence

1. Allocate and populate a module page while it is RW+NX in the identity map.
2. Transition its identity alias from RW+NX to RO+NX.
3. Map the physical page into the module arena as supervisor RO+X.
4. On module teardown, unmap the module alias first.
5. Only then restore the identity mapping to RW+NX, if the owner still holds
   the exact expected-permission state.

This phase only provides the generic identity permission primitive. The KEXT
loader does not yet invoke this sequence for the Intel driver and does not
execute its entry point.

## Limitations

- The identity alias operation is limited to physical addresses below 4 GiB,
  matching the current kernel identity-map and managed-page allocator range.
- The operation updates the shared low identity subtree and performs local
  `INVLPG`. It is not an SMP-wide TLB shootdown. The current address-space
  switching model remains single-CPU for this phase.
- The API checks the direct identity alias and the reserved module arena. It
  does not scan every possible arbitrary kernel virtual alias. Therefore this
  is not a global proof that no writable alias exists anywhere in the kernel.
- Huge-page demotion allocates page-table pages from the existing managed-page
  allocator. A failed later permission update may leave a semantically
  equivalent but more finely split page-table structure.
- No Intel PCI configuration writes, BAR sizing, MMIO mapping, GPU register
  access, KEXT entry execution, or native acceleration is performed here.

## Verification

`tests/test_kernel_identity_permissions_phase5.sh` runs source-level contract
checks and the Clang ASan/UBSan host self-test. The expanded combined
kernel-stack/module-mapping self-test contains 94 checks and is also run with
GCC ASan/UBSan for the Phase 5 validation record. Kernel and external Intel
KEXT builds are regression checks only; they do not execute the KEXT.
