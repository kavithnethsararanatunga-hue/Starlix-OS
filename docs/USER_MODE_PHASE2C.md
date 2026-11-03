# Starlix OS User Mode — Phase 2C: Controlled CR3 Activation

## Scope

Phase 2C adds a controlled, single-CPU CR3 activation API on top of the Phase 2B
four-level address-space manager. It does not enable Ring 3, SYSCALL/SYSRET,
scheduling, context switching, or user-process startup.

## API

- `address_space_activate(const AddressSpace&)`: activate a registered process
  address-space root from Ring 0.
- `address_space_activate_kernel()`: restore the original kernel root.
- `address_space_active_root()`: report the manager's tracked active root.

The hardware adapter saves RFLAGS and disables maskable interrupts before taking
the address-space lock. It verifies CPL=0, IF=0, CR4.PCIDE=0, the expected current
CR3 root, and valid low/reserved CR3 bits before writing CR3. It verifies the
resulting root and attempts to restore the prior root if readback does not match.
The caller's original IF state is restored after the lock is released.

The original CR3 PWT/PCD control bits are preserved. PCID is explicitly rejected
until a PCID-aware allocator, invalidation strategy, and lifecycle are designed.
A CR3 reload flushes non-global TLB translations; inherited kernel mappings are
copied unchanged except that U/S is cleared in the cloned PML4 entries. User leaf
mappings are never marked global.

## Page-table mutation rule

`address_space_map_user_page` and `address_space_unmap_user_page` reject edits to
the currently active address space. This phase has no INVLPG/TLB-shootdown API.
Create and populate a process address space while inactive, activate it only
after its mappings are complete, and switch back to the kernel root before
editing it again.

## Preconditions and limitations

- Four-level paging only; LA57 is rejected.
- CR0.PG and CR0.WP, CR4.PAE, EFER.LMA and EFER.NXE must be enabled.
- PCIDE must be clear; CR3 PWT/PCD are the only accepted low control bits.
- Page-table pages must be identity-accessible below 4 GiB in the current kernel
  memory model.
- All cloned roots must retain the same supervisor-only kernel mappings.
- This is BSP/single-CPU only. There is no SMP lock protocol or TLB shootdown.
- No scheduler integration, TSS RSP0 update, interrupt-origin tracking, or
  process/thread ownership integration is provided yet.
- No Ring 3 launch, syscall activation, ECT loader, or `.app` launch is enabled.
- Live UEFI page-table compatibility and actual VMware CR3 switching remain
  unverified until a dedicated boot test is performed.

## Validation

`tests/test_address_space_phase2c.sh` uses a host-side simulated CR3 backend and
ASan/UBSan. It tests process-root activation, no-op same-root activation,
interrupt-state preservation, failure rollback/bookkeeping, CPL/PCIDE rejection,
unexpected CR3 detection, kernel-root restoration, and refusal to edit an active
address space. These tests do not execute privileged `mov %cr3` on hardware.
