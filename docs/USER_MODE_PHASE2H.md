# Starlix OS Phase 2H — Opt-in Ring-3 Hardware Smoke Test

## Scope

Phase 2H adds an explicitly opt-in hardware bring-up test for the Phase 2A–2G
user-mode foundations. It is not the production ECT loader, a general syscall
ABI, authentication, a lock screen, or a desktop environment.

Normal builds keep `STARLIX_ENABLE_RING3_SMOKE_TEST=0`: they do not install the
DPL3 test gate and do not start a user thread. The test must be deliberately
enabled for a separate VMware test image.

## Test design

The test prepares two independent x86-64 user address spaces and two registered
user processes. Each process has:

- its own RX user-code page;
- its own 16-page RW/NX user stack;
- its own managed guarded kernel stack from the Phase 2G stack arena;
- a mapping of the same physical counter page at the same user virtual address.

The two small position-independent probe payloads update different qwords in
the shared counter page. The first payload cannot finish until the peer counter
has also reached the threshold. The ready queue contains both threads at equal
priority. The first user thread enters through the existing validated IRETQ
path; a CPL3 PIT IRQ0 must preempt it and select the second user thread through
the Phase 2F CR3/TSS.RSP0/FPU/FS/GS switch path.

When both counters reach the threshold, the running payload executes `INT
0x80`. Only in this opt-in build, IDT vector 0x80 is a DPL3 interrupt gate. Its
assembly stub performs `SWAPGS`, builds the normalized interrupt frame and
calls a noreturn kernel validator. The validator checks the test magic, exact
saved RIP, current registered user thread, process state, active CR3, TSS.RSP0,
registered kernel-stack ownership, at least one successful timer-driven switch,
and both user-written counters. It prints one PASS or FAIL line to COM1 and
halts the CPU. This is a test-only exit trap, not a user-visible system-call
service.

`EFER.SCE` remains clear. No `SYSCALL/SYSRET`, generic INT 0x80 service table,
user exception recovery, AVX/XSAVE, or production process-launch interface is
enabled by this phase.

## Build modes

Normal production/development build (smoke test disabled):

```bash
bash scripts/build_kernel.sh
```

Opt-in test kernel and complete boot disk image:

```bash
STARLIX_ENABLE_RING3_SMOKE_TEST=1 bash scripts/build_boot_kernel.sh
```

The full image script repacks `RxKernel1.0.ect` and rewrites build artifacts
under `image/` and `ssp_root/`. Back up the current image and any local changes
before running it. Use a disposable VMware clone/snapshot for this test because
the successful or failed user-mode test intentionally halts the CPU.

For an explicit normal rebuild afterward:

```bash
STARLIX_ENABLE_RING3_SMOKE_TEST=0 bash scripts/build_boot_kernel.sh
```

## Expected VMware serial evidence

Before the first IRETQ:

```text
[RING3-TEST] Launching two isolated probe processes; waiting for timer preemption and INT 0x80 completion.
```

Successful completion:

```text
[RING3-TEST] PASS: CPL3 IRETQ, user memory isolation, shared counter page, PIT preemption, alternate IRETQ frame, CR3 and TSS.RSP0 verified.
```

If preparation fails before Ring 3, the kernel logs `NOT STARTED` and remains
in Ring 0. If an invariant fails after entry, the test prints `FAIL` and halts.
A reset is then required.

## Validation limits

Host ASan/UBSan tests validate the payload builder, its address bounds and
machine-code layout. Static contract tests validate the opt-in gate and
integration wiring. Kernel and EFI builds prove compile/link compatibility.
Only the VMware run can confirm the real CPU behavior of `IRETQ`, `SWAPGS`,
PIT preemption, live CR3 changes, TSS.RSP0 transitions and user page-table
permissions. Do not mark Ring-3 execution verified until the expected serial
PASS line is observed from the actual VM.
