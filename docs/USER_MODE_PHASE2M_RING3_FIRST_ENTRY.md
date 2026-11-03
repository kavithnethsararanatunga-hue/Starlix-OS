# Starlix OS 1.0 Beta — Phase 2M: Ring 3 First-Entry Integration

## Goal

Provide an explicit, fail-closed bridge from the Phase 2L ECT-backed user
process builder to the existing x86-64 first-user IRETQ path. Process creation
and user execution remain separate operations. The normal boot path does not
start an ECT process.

## Implemented

- `scheduler_is_admitted()` validates that a registered READY thread is present
  in the bounded ready set by both TID and pointer identity.
- `StNtStartUserProcess()` validates the opaque process handle, resource cookie,
  PID/TID, CR3, ECT entry, stack pointer, completed resource transaction,
  READY process/thread states, exact scheduler admission, initialized/idle
  context-switch coordinator and active kernel CR3.
- First-user entry revalidates the target frame, ECT entry mapping, user stack,
  guarded kernel stack and thread ownership before changing privileged CPU state.
- The transition saves the kernel FPU/SSE state, installs the target TSS.RSP0,
  activates the process CR3, restores the target FPU/SSE state, sets and verifies
  FS_BASE, GS_BASE and KERNEL_GS_BASE, removes the READY scheduler entry, commits
  process/thread RUNNING state, arms the context-switch coordinator and enters
  through the existing common IRETQ restore path.
- Before the lifecycle commit, errors restore the original CR3, TSS.RSP0, FPU
  state and FS/GS MSRs. If the restoration itself cannot be verified, the CPU
  fail-stops with interrupts disabled rather than returning in an unknown
  privilege-transition state.
- If a rejected start had already removed its READY scheduler slot, process
  rollback recognises that exact absent-slot state and can still clean up the
  unpublished process.
- `context_switch_switch_count()` is declared in the public context-switch
  header so the opt-in diagnostic harness can compile against its implementation.
- Phase 2L's creation contract now checks that create and start remain separate.

## Public process API

```cpp
UserProcessHandle handle{};
if (!StNtCreateUserProcessFromECT(name, ect_bytes, ect_size, &handle)) {
    // Creation did not complete; inspect the returned handle/transaction state.
    return false;
}

// Successful first-user entry does not return (IRETQ enters CPL3).
if (!StNtStartUserProcess(&handle)) {
    // Start was rejected and the prepared process remains abortable.
    (void)StNtAbortUserProcess(&handle);
    return false;
}
```

The process builder itself never launches the thread. The loader/session manager
must call `StNtStartUserProcess()` explicitly after its own policy checks.

## Tests and build validation

- Phase 2M start-policy host test: 9/9 with Clang ASan+UBSan and GCC ASan+UBSan.
- Phase 2M first-entry source/assembly contract: 11/11.
- Phase 2M start API integration contract: PASS.
- Phase 2D scheduler self-test: 36/36 (includes pointer-identity membership).
- Phase 2E interrupt-frame self-test: 33/33; IRQ return contract: 6/6.
- Phase 2F context-switch policy: 28/28 with Clang and GCC; IRQ contract: 27/27.
- Phase 2H user probe payload: 27/27 with Clang and GCC; IRQ contract: 26/26.
- Phase 2I–2L component, loader, mapping, stack, process-transaction and abort
  regressions passed in the reconstructed Phase 2L tree.
- Normal kernel build: ELF64 x86-64, entry `0x200000`, `kernel.bin` 2,441,776
  bytes including BSS, no undefined symbols. The test-only Ring 3 probe exit
  dispatcher is absent from the default kernel.
- Opt-in Ring 3 diagnostic kernel build: PASS; the test-only probe-exit
  dispatcher is present only when `STARLIX_ENABLE_RING3_SMOKE_TEST=1`.
- EFI stages build: PASS (`BOOTX64.EFI`, `stbtmngr.efi`, `Rxload.efi`, `boot.efi`).

## Runtime gate and limitations

This package does not assert that a physical CPU or VMware has executed IRETQ,
SWAPGS, CR3 writes, FXSAVE/FXRSTOR or timer preemption. The opt-in diagnostic
kernel is a test-only two-process user probe; it is not `stlcs.ect` and its
`INT 0x80` gate is not a general syscall ABI.

To run the existing diagnostic in a disposable VM snapshot, the user's WSL
environment can build the opt-in disk image with:

```bash
STARLIX_ENABLE_RING3_SMOKE_TEST=1 bash scripts/build_boot_kernel.sh
```

The VM COM1 serial output must contain this exact line before claiming
hardware/VMware Ring 3 verification:

```text
[RING3-TEST] PASS: CPL3 IRETQ, user memory isolation, shared counter page, PIT preemption, alternate IRETQ frame, CR3 and TSS.RSP0 verified.
```

The full disk-image step requires `sgdisk`, `mkfs.vfat`, `mmd` and `mcopy`.
These utilities are not available in the isolated build container used for this
patch, so no bootable image or VMware runtime test is claimed here.

Still not implemented by Phase 2M:

- `stlgn.ect` host-service bridge and lifecycle integration.
- LSASS authentication service and secure credential protocol.
- General-purpose syscall ABI or recoverable user-mode exceptions.
- GUI/input presentation bridge for the standalone `stlcs.ect` component.
- RXFS system-volume image staging and automatic boot-time process launch.
- A real VMware/physical-device run of the new ECT process-start API.

The existing context-switch backend remains single-CPU, legacy FXSAVE/FXRSTOR
only, without SMP TLB shootdown, AVX/XSAVE or PCID support. The existing
physical-to-virtual adapter remains constrained to its documented identity
mapped physical range.
