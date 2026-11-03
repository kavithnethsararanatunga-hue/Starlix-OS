# Starlix OS — User Mode Phase 2L: ECT User Process Creation

## Scope

Phase 2L adds a fail-closed process preparation path for a validated ECT v1
image. It creates an inactive address space, maps the executable, creates a
user stack, allocates a guarded kernel stack, registers a process and READY
main thread, binds the kernel stack and admits the thread to the scheduler.

**Phase 2L does not enter Ring 3.** It does not start the scheduler, perform a
context switch, execute `stlcs.ect`, or present a graphical lock screen. The
caller receives a prepared process handle which must remain inactive until a
separate, explicitly reviewed first-user-entry phase is completed.

## New source files

- `kernel/src/process/user_process_transaction.hpp/.cpp` — backend-driven,
  reverse-order resource transaction and retryable rollback.
- `kernel/src/process/user_process.hpp/.cpp` — kernel backend connecting the
  transaction to address spaces, ECT mapping, managed pages, kernel stacks,
  process/thread registries and scheduler admission.
- `tests/user_process_transaction_phase2l_selftest.cpp` and
  `tests/test_user_process_transaction_phase2l.sh` — host failure-injection
  tests for all transaction stages.
- `tests/process_abort_phase2l_selftest.cpp` and
  `tests/test_process_abort_phase2l.sh` — process/thread registry rollback tests.

Phase 2L also extends address-space destruction and kernel-stack release, and
updates the Phase 2G / Phase 2K host tests to cover the new reclamation paths.

## Public API

`StNtCreateUserProcessFromECT(name, bytes, size, &handle)` requires:

- Physical page allocator, address-space manager and kernel-stack manager ready.
- Scheduler initialized.
- Kernel address space active.
- No current user thread and no first-user context switch already armed.
- A bounded, null-terminated image name and a valid ECT v1 byte range.

The transaction order is:

1. Create an inactive address space.
2. Validate and map ECT image sections via the Phase 2J/2K loader.
3. Allocate and map 16 user RW/NX stack pages.
4. Allocate a guarded supervisor RW/NX kernel stack.
5. Create the process and READY main thread.
6. Bind the kernel stack and prepare its initial IRET frame.
7. Admit the READY thread to the scheduler.

The stack pointer is 16-byte aligned and reserves two zeroed qwords at the
initial stack top. This is the current ECT entry ABI, not a System V or POSIX
startup ABI. The image mapper currently relies on the existing identity-map
adapter and fails closed for physical pages at or above 4 GiB.

The API does not activate the process CR3. It does not call
`context_switch_start_first_user()`.

`StNtAbortUserProcess(&handle)` removes a prepared but not-running process.
It removes scheduler admission first, detaches/releases its kernel stack,
removes the unpublished thread/process registry entries, unmaps/releases the
user stack and image, then destroys the now-empty inactive address space. If
any release callback fails, the handle retains the residual transaction state
so cleanup can be retried. Non-LIFO process-registry removal is rejected.

## Memory and safety invariants

- ECT image pages retain Phase 2J/2K section permissions and W^X enforcement.
- User stack pages are user RW/NX and bounded to the reserved user region.
- Kernel stacks remain supervisor RW/NX with two unmapped guard pages.
- Physical pages are returned through managed page leases.
- Kernel stack virtual arena addresses are monotonic and are not recycled;
  released physical pages can be reused.
- A live or RUNNING thread's kernel stack cannot be released.
- Address spaces can be destroyed only when inactive, registered, empty of
  user mappings and free of private supervisor mappings.
- Scheduler admission is the final transaction stage. A failed stage rolls
  back in reverse order; incomplete rollback remains retryable.

## Build and tests

```bash
bash tests/test_user_process_transaction_phase2l.sh
CXX=g++ bash tests/test_user_process_transaction_phase2l.sh
bash tests/test_process_abort_phase2l.sh
CXX=g++ bash tests/test_process_abort_phase2l.sh
bash tests/test_kernel_stack_phase2g.sh
CXX=g++ bash tests/test_kernel_stack_phase2g.sh
bash tests/test_address_space_phase2k_reclaim.sh
CXX=g++ bash tests/test_address_space_phase2k_reclaim.sh
bash scripts/build_kernel.sh
```

The kernel build keeps `STARLIX_ENABLE_RING3_SMOKE_TEST=0` by default.

## Remaining integration work

A later phase must add a safe, reviewed first-user-entry call site and its
lifecycle/termination policy before any prepared user thread is run. The
`stlgn.ect` host-service bridge, LSASS authentication service, graphics/input
services, RXFS boot-image staging and the actual VMware Lock Screen runtime
are still separate work. Passing Phase 2L tests does not prove that the lock
screen runs in VMware.
