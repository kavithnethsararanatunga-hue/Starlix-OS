# Starlix User Mode Phase 2G — Guarded Kernel Stacks

## Implemented

- Reserve PML4 slot 510 (`0xFFFFFF0000000000` .. `0xFFFFFF8000000000`) for a shared kernel-stack virtual arena. Initialization fails closed if firmware already occupies that slot.
- Every cloned process root copies the supervisor-only, writable, NX PML4 entry; all roots share the same lower page-table subtree.
- Kernel stacks use contiguous physical pages from the existing UEFI conventional-memory allocator, then map those frames at dedicated high-half virtual addresses.
- Stack mappings use supervisor-only, writable, NX intermediate and leaf entries. Each stack has one unmapped guard page below and above.
- Bounded boot-lifetime registry (128 stacks), allocation descriptor identity, per-thread owner TID, and mapping/guard validation.
- A stack must be allocated, supplied as `kernel_stack_top` to `StNtCreateThread`, then bound with `kernel_stack_bind_thread()` before context-switch admission.
- Context-switch validation rejects caller-owned, unbound, incorrectly mapped or wrong-process kernel stacks.

## Current limits

- Physical allocator has no free/reclaim API; stack physical pages and virtual slots are boot-lifetime.
- Single CPU only; no SMP shootdown. The shared mapping is modified only for fresh, previously unused virtual addresses while the local interrupt state is preserved.
- PML4 slot 510 must be empty in the inherited UEFI page tables. If not, address-space initialization fails closed and user-thread switching stays disabled.
- Kernel stacks are allocated on demand, not automatically by `StNtCreateThread`; the future ECT/session loader must use the allocate → create-thread-with-top → bind sequence.
- No VMware or physical hardware boot test was performed in this phase. Host ASan/UBSan tests validate mapping/ownership contracts, not real page-table execution under the CPU.
- Ring 3 remains disarmed until a validated executable/session launcher calls `context_switch_start_first_user()`.
