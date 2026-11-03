# Starlix OS User Mode — Phase 2A Foundation

Status: **implemented and host/kernel-build validated; Ring 3 remains disabled**.

## Purpose

Phase 2A replaces the old singleton process/thread placeholders with a bounded
executive registry and defines the first user virtual-address and page
permission contract. It does not switch CR3, create hardware page-table trees,
load ECT images, schedule threads, or enter CPL3.

## User RXFS paths

The target system component paths are:

- `RXFS/sys/User/smgrsb.ect`
- `RXFS/sys/User/stitzn.ect`
- `RXFS/sys/User/lsass.ect`
- `RXFS/sys/User/stlgn.ect`

The target Explorer executable remains:

- `RXFS/Applications/Explorer.app/Contents/Starlix/Explorer.ect`

`RXFS/sys/BaseSystem.dmg` remains the UDIF/APFS boot image. Its APFS
`System/Library/Extensions/*.kext` and `System/Kernels/Kernel` layout is not
changed by this phase.

## User virtual-address contract

- 4-level, 48-bit canonical x86-64 virtual-address model.
- PML4 slot 1 is reserved for the initial user region:
  `0x0000008000000000` through (exclusive) `0x0000010000000000`.
- Initial image placement begins at `USER_IMAGE_BASE` (region base + 1 MiB).
- Initial user stack reservation is 16 pages below `USER_STACK_TOP`.
- User ranges must be non-empty, canonical, non-overflowing, and wholly inside
  the reserved region.
- User leaf PTE contract requires Present + User; writable and executable
  cannot both be enabled (W^X). Non-executable mappings require NX support.
- These are validation/layout helpers only. No live PTEs or CR3 are modified.

Before Phase 2B uses PML4 slot 1, it must inspect the inherited UEFI page tables
and fail closed if that slot is already occupied or its lower-level mappings
cannot safely be cloned. The design must preserve all existing kernel/firmware
mappings as supervisor-only.

## Process/thread registry

- PID 4 is reserved for `System`; new user PIDs begin at 5.
- Maximum 32 process records (including System).
- Maximum 128 thread records globally and 16 per process.
- Process and thread records are separate persistent slots; terminated slots
  are not reused because reclamation/handle invalidation is not implemented.
- A user process must be supplied a non-zero, page-aligned CR3 value within the
  supported physical-address field.
- A thread must have a user entry point and user stack pointer in the reserved
  user region, plus a canonical, 16-byte-aligned supervisor kernel stack top.
- Each live thread must own a unique kernel stack top.
- Initial user selectors are CS=0x23 and SS=0x1B; initial RFLAGS is 0x202.
- Process termination terminates its remaining threads.
- State transition functions reject invalid transitions and stale/foreign
  object pointers.

The caller must still allocate and map the actual page-table pages, user stacks,
and kernel stacks before a thread can run. Phase 2A only records and validates
these values.

## Explicitly disabled / not implemented

- `EFER.SCE` remains clear; `SYSCALL` is not enabled for user execution.
- `jump_to_user_mode` remains a fail-stop stub.
- The exception dispatcher still implements kernel-fatal exception handling;
  user-fault recovery/termination must be implemented before CPL3 entry.
- No scheduler/context switch, user page-table cloning/mapping, kernel-stack
  allocator, process teardown/reclamation, ECT loader, IPC security boundary,
  credential service, lock screen, or `.app` launcher is added here.

## Tests

Run:

```bash
bash tests/test_user_mode_phase2a.sh
```

The test compiles the exact production process registry and shared address
contract under AddressSanitizer + UndefinedBehaviorSanitizer. It covers address
canonicality/range overflow, page-span arithmetic, PTE policy/W^X, process and
thread uniqueness, stack ownership checks, bounded capacity, lifecycle
transitions, and process-to-thread termination propagation.
