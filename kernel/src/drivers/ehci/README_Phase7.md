# Starlix OS EHCI Phase 7 — Legacy IRQ Notifications

Copyright © 2026 OpenRPX xsOft. All rights reserved.

## What this phase adds

- Installs dedicated IDT stubs for legacy PIC IRQ vectors `0x20`–`0x2F`.
- Saves/restores general-purpose registers around a common IRQ entry and dispatches to a registered short C handler.
- Adds a basic 8259 PIC IRQ registration API with masking/unmasking and master/slave EOI handling, including spurious IRQ 7/15 checks.
- Reads the PCI EHCI function's interrupt-line field and enables EHCI USBINT, USBERRINT, PORT CHANGE, HOST ERROR, and Async-Advance notifications only if the line is a plausible remapped PIC IRQ. Otherwise it keeps the previous bounded-polling path.
- Sets IOC on the terminating control-status and bulk qTD so EHCI can notify the CPU of transfer completion.
- Keeps the EHCI callback short: it only acknowledges EHCI write-one-to-clear cause bits and updates event counters. It does not log, allocate memory, enumerate devices, or block.
- Prevents the Async-Advance doorbell waiter from falsely timing out when the IRQ callback acknowledges IAA before the polling code observes the USBSTS bit.
- Includes structural checks plus a non-destructive QEMU regression script using a disposable MBR/FAT32 virtual USB disk.

## Important limitations

**This is an interrupt foundation, not yet fully asynchronous transfer scheduling and not Windows-class USB support.** Existing control/bulk APIs still wait on qTD state with bounded polling. The interrupt callback collects/acknowledges causes but does not wake a scheduler-backed wait queue because Starlix does not yet have that request scheduler.

This phase supports the legacy 8259 PIC path only. It does not implement IOAPIC routing, MSI/MSI-X, APIC setup, interrupt affinity, interrupt nesting, a timer service, fully diagnosed exception handlers, or hot-plug lifecycle. PCI `Interrupt Line` metadata can be absent or inaccurate on some machines; if so, the driver logs that it is retaining bounded polling. The default IDT stub now fails safe by halting on an unhandled vector rather than attempting to `iretq` through an unknown exception frame.

Phase 6's DMA limitation remains: DMA addresses still rely on Starlix's current identity-addressed static-memory convention. There is no general page-backed DMA allocator, verified virtual-to-physical translation, IOMMU integration, or cache-maintenance backend.

## Install

Download this ZIP to `~/Downloads`, then run:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
bash ~/Downloads/Starlix_EHCI_Phase7_Patch/install_ehci_phase7.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

Use `--kernel-only` to run source checks and build the freestanding kernel without repackaging ECT/EFI/SSP/disk images.

The installer backs up the touched source files, kernel build output, EFI build output, SSP tree, and image/package artifacts. It restores them if source checks, kernel linking, or packaging fails.

## Runtime regression

After a successful install, run:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
bash ~/Downloads/Starlix_EHCI_Phase7_Patch/test_ehci_phase7_qemu.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta
```

The script creates a disposable 64 MiB USB image with an MBR partition and FAT32 files. It never uses a physical USB disk. Log: `image/ehci-phase7-qemu.log`.

The script reports whether the EHCI IRQ callback count was observed. A zero count is a warning, not an interrupt test pass; it may mean the platform's PCI interrupt routing does not use the configured legacy PIC line. The FAT32 regression is checked separately.

## Validation performed while preparing this patch

- All 13 structural source checks passed.
- IDT assembly compiled.
- Freestanding kernel compiled and linked from the latest Library `1.0.Beta.zip` source snapshot.
- The full ECT/EFI packaging pipeline was started; EFI build and SSP preparation worked in this environment, but final disk-image packaging could not be executed here because `mkfs.vfat` is not installed in the packaging environment. Run the installer on the Starlix Ubuntu machine to validate complete packaging and then run the QEMU test.
- Physical hardware validation has not been performed.
