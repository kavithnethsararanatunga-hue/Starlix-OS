# Starlix OS EHCI Phase 6 — DMA Range and Ordering Hardening

Copyright © 2026 OpenRPX xsOft. All rights reserved.

## What Phase 6 changes

- Adds `ehci_dma_guard.hpp`, a small, host-testable guard for 32-bit EHCI DMA regions.
- Validates the *entire* QH, qTD, frame-list and 4 KiB bounce-buffer range, not only the first byte/address. It rejects null, misaligned, zero-length, out-of-range and 4-GiB-crossing regions.
- Adds x86 `sfence`, `lfence`, and `mfence` helpers for descriptor publication, completion reads, async-advance safe points, and controller-halt safe points.
- Publishes each transfer's QH `next_qtd` pointer only after its qTD, bounce buffer, and remaining QH overlay fields have been prepared.
- Keeps the current static, serialized transfer model and current 32-bit EHCI addressing assumption.
- Includes host tests for boundary cases and a QEMU MBR/FAT32 regression script.

## Important limitation — not full physical DMA mapping

This is a DMA safety foundation, not a general DMA mapper. The current Starlix memory subsystem does not yet provide a verified virtual-to-physical translation API or a page-backed DMA allocator that reserves pages against the UEFI memory map. `to_identity_address32()` therefore validates the current kernel's *identity-addressed* pointer convention and does not translate addresses. Keep this driver experimental until Phase 6 follow-ups add a real physical allocator/mapping API and those paths are used by the EHCI driver. These x86 fences also assume the coherent-memory behavior of the targeted x86 platforms; they do not replace cache maintenance on non-coherent architectures.

No interrupts, hubs, hot-plug, concurrency-safe request queues, dynamic DMA allocations, IOMMU support, or USB 3.x/xHCI support are introduced in this phase.

## Install

Download this ZIP to `~/Downloads`, then run:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
bash ~/Downloads/Starlix_EHCI_Phase6_Patch/install_ehci_phase6.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

Use `--kernel-only` to build just the freestanding kernel and skip ECT/EFI/SSP/disk packaging.

The installer runs host-side range checks, records a timestamped backup, applies the source changes, builds the kernel, and packages the image. Build/package failure triggers source/build/artifact rollback. Review the backup path printed by the installer.

## Runtime regression test

After a successful install, run:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
bash ~/Downloads/Starlix_EHCI_Phase6_Patch/test_ehci_phase6_qemu.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta
```

This uses a disposable 64 MiB virtual USB disk with one MBR FAT32 partition and read-only file tests. It never uses the host's physical USB drive. Log: `image/ehci-phase6-qemu.log`.

## Validation status of this package

- DMA range helper tests: run with `-Wall -Wextra -Werror`.
- EHCI source transformation: tested against the Phase 2 driver snapshot and compiled in freestanding syntax-only mode using test stubs.
- Full build against the user's current source tree and QEMU runtime: must still be done by running the installer and regression script on the Starlix development machine.
