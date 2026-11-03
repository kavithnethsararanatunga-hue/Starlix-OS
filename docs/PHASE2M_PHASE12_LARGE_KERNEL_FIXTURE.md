# Starlix Phase 2M / Phase 12 Large Production-Kernel Fixture

## Purpose

Keep the existing Phase 12 production-candidate pipeline, but raise its test
fixture limits enough to carry the Phase 2M opt-in Ring-3 diagnostic kernel ECT.
The Phase 10 RXFS disk is used as an input and is copied to a disposable output;
the original Phase 10 image is not modified.

## Capacity changes

- Production ECT maximum: 4 MiB (the current Phase 2M ECT is about 2.45 MB).
- APFS fixture: 4 MiB (1,024 4-KiB blocks).
- UDIF/BaseSystem test envelope: 5 MiB.
- Synthetic RXFS fixture: 16 MiB.
- RXFS fixture now creates single-indirect and double-indirect pointer tables and
  re-reads the entire BaseSystem.dmg and full kernel ECT through RXFS -> UDIF -> APFS.
- The synthetic RXFS fixture is checked against the Phase 10 RXFS partition size
  before the candidate script writes it into the disposable disk copy.

## Build

From the project root under WSL:

```bash
bash tests/test_phase2m_phase12_capacity.sh
bash tools/build_starlix_phase2m_phase12_candidate.sh \
  "$PWD/build/candidate_rxfs_phase10/run_20261027_215640"
```

The wrapper first backs up existing generated kernel/ECT/EFI build outputs under
`.backups/phase2m-phase12-artifacts-*`. It performs an opt-in Phase 2M Ring-3
diagnostic kernel build, packs
`kernel/build/kernel.bin` into `build/RxKernel1.0.phase2m-ring3.ect`, verifies the
ECT metadata, exact payload and CRC32, then calls the existing
`tools/build_stbootmngr_phase12_runtime_candidate.sh` with that ECT.

Required candidate tools are checked by the existing Phase 12 script, including
`sgdisk`, `mcopy`, `mmd`, `dd`, `clang++`, and `lld-link`.

## Runtime scope

This candidate runs the Phase 2H two-process Ring-3/context-switch diagnostic
compiled into the Phase 2M kernel. The diagnostic's `INT 0x80` gate is test-only;
it is not a general syscall ABI. This test does **not** yet launch `stlcs.ect`,
perform LSASS authentication, provide the `stlgn.ect` services bridge, or show
the graphical Lock Screen.

Use a disposable VMware clone/snapshot. The test intentionally stops the guest
on a validated PASS or FAIL. Confirm the serial marker:

```text
[RING3-TEST] PASS: CPL3 IRETQ, user memory isolation, shared counter page, PIT preemption, alternate IRETQ frame, CR3 and TSS.RSP0 verified.
```

A successful compile, ECT round-trip, or host self-test is not a substitute for
this VMware serial result.
