# Starlix OS User Mode — Phase 2E

## Interrupt-return frame and per-thread kernel-stack foundation

Phase 2E adds the x86-64 frame contract needed for a future interrupt-driven
thread switch. It deliberately does **not** enable scheduling, launch Ring 3,
or return an alternate frame from the live IRQ dispatcher.

### IRQ frame ABI

For an IRQ taken from CPL3, the CPU pushes `SS, RSP, RFLAGS, CS, RIP`. The
common PIC stub then pushes a vector number followed by these general-purpose
registers in this order: `RAX, RCX, RDX, RBX, RBP, RSI, RDI, R8..R15`.
The resulting low-to-high stack layout is represented by
`cpu/interrupt_frame.hpp` and has a fixed size of 168 bytes:

| Offset | Field |
|---:|---|
| 0..112 | `R15, R14, R13, R12, R11, R10, R9, R8, RDI, RSI, RBP, RBX, RDX, RCX, RAX` |
| 120 | vector |
| 128 | RIP |
| 136 | CS |
| 144 | RFLAGS |
| 152 | user RSP |
| 160 | user SS |

`starlix_irq_dispatch(vector, frame)` now returns a 64-bit stack pointer. Zero
means resume the original interrupt frame. A non-zero value tells the assembly
epilogue to restore from a different frame. The C++ dispatcher currently
always returns zero, so the existing IRQ behavior is preserved. A future
scheduler must fully validate any alternate frame before returning it.

### First user-return frame

`thread_prepare_user_interrupt_frame()` places a zeroed 168-byte initial frame
immediately below a caller-owned kernel stack top. It validates:

- the thread is registered and in READY state;
- the stack base and size are page aligned, bounded, canonical, supervisor
  addresses, and the stack top matches the value supplied at thread creation;
- the stack does not overlap another thread.s already-prepared kernel stack;
- the entry point and user stack are within the reserved Starlix user region;
- the frame uses the Starlix user CS/SS selectors, RFLAGS `0x202`, and a valid
  IRETQ return state.

This API only prepares a frame. It does not verify physical backing/page-table
mappings for the caller-owned kernel stack, launch the thread, enable user mode,
or schedule it.

### TSS RSP0

`tss_set_kernel_stack_checked()` validates an aligned canonical supervisor
stack top before writing TSS.RSP0. `tss_get_kernel_stack()` supports later
switch validation. The existing bootstrap setup now checks the setter result.
The legacy void setter remains as a compatibility wrapper.

### Validation and current limitations

`tests/test_interrupt_frame_phase2e.sh` runs ASan/UBSan host tests for frame
size/offsets, initial frame construction, selectors, user address checks,
RFLAGS restrictions, stack bounds/non-overlap, and TSS RSP0 validation.

The following are not implemented in Phase 2E:

- scheduler-driven IRQ frame selection (dispatcher returns zero);
- first-thread IRETQ launch or actual Ring 3 execution;
- timer preemption or real task switching in VMware/hardware;
- CR3/TSS switching as one scheduler transaction;
- FPU/SSE/AVX state save/restore (kernel remains GPR-only);
- syscall enablement, ECT loading, authentication, or desktop startup.

Do not enable user threads or make the dispatcher return a non-zero frame until
Phase 2F implements and validates the complete switch transaction and FPU
state policy.
