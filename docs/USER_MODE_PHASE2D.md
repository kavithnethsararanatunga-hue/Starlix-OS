# Starlix OS User Mode — Phase 2D Scheduler Policy Core

## Scope

Phase 2D adds a bounded, host-testable ready-set and scheduler selection policy.
It is a policy foundation only. It does **not** perform hardware context switches,
load CR3, change TSS.RSP0, program timer quantum/preemption, or launch Ring 3.

## Policy

- Fixed capacity: 64 ready entries (below the 128-thread executive limit).
- Priorities: integer 0..31; 31 is highest.
- Strict priority selection: highest-priority runnable READY thread wins.
- Round-robin among equal-priority threads using a monotonically increasing
  admission/dispatch sequence.
- A thread is admissible only when it is a registered object, is READY, has a
  live parent process (READY or RUNNING), and has a supported priority.
- Blocked/terminated threads and threads owned by terminated processes are
  pruned when selecting a candidate.
- Explicit remove and priority-change APIs are bounded and allocation-free.

## Safety boundary

`scheduler_pick_next()` only returns a candidate. It deliberately leaves all
process/thread states unchanged. A later context-switch phase must atomically
save the outgoing interrupt frame, select/validate the next thread, update its
TSS kernel stack, switch address spaces where needed, and restore its complete
register/iret frame before this policy becomes an execution scheduler.

The current IRQ stubs do not yet provide a scheduler-owned privilege-transition
frame contract, and TSS.RSP0 is currently static. Therefore this phase does not
register a timer preemption hook or claim that a selected user process runs.

## Paths

- `kernel/src/process/scheduler.hpp`
- `kernel/src/process/scheduler.cpp`
- `kernel/src/process/process.hpp`
- `kernel/src/process/process.cpp`
- `scripts/build_kernel.sh`
- `tests/scheduler_phase2d_selftest.cpp`
- `tests/test_scheduler_phase2d.sh`
