# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.

# Starlix OS 1.0.Beta — EHCI Phase 2 patch

Phase 2 advances the existing polling EHCI driver from basic enumeration to high-speed bulk transfers and a USB Mass Storage Class (MSC) Bulk-Only Transport (BOT) probe.

## Included

- A bounded, single-qTD bulk transfer API with a 4096-byte DMA bounce buffer.
- Per-device/per-endpoint/per-direction bulk data-toggle tracking.
- Descriptor parsing for USB Mass Storage / SCSI / Bulk-Only Transport interfaces.
- BOT CBW/data/CSW command path and validation.
- SCSI INQUIRY and READ CAPACITY(10) during USB storage binding.
- A non-destructive READ(10) LBA 0 self-test after capacity discovery.
- Public synchronous `ehci_msc_read_blocks()` and `ehci_msc_write_blocks()` primitives. Requests are capped at 4096 bytes; the write API is provided for later block-layer integration and is not automatically called by boot.
- Timestamped source/build/image backups before install.

## Apply on Ubuntu

Download and extract the ZIP, then run this from the extracted directory:

```bash
bash ./install_ehci_phase2.sh /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

To build just the kernel before repackaging:

```bash
bash ./install_ehci_phase2.sh /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --kernel-only
```

The installer stores a source/build/artifact backup under `.backups/ehci-phase2-<timestamp>/`. It builds the kernel and, in `--image` mode, rebuilds ECT/EFI/SSP and the disk image.

## Expected QEMU log additions

For a USB mass-storage device implementing the BOT protocol, successful logs should include:

```text
[EHCI][MSC] USB Mass Storage BOT interface found.
[EHCI][MSC] SCSI INQUIRY: ...
[EHCI][MSC] Capacity ready: last LBA ..., block size ... bytes.
[EHCI][MSC] READ(10) LBA 0 succeeded; block data reached kernel memory.
```

The exact device string/capacity depends on the attached device. If these messages do not appear, share the full serial log rather than assuming success from the kernel build alone.

## Important limitations

This is still not a Windows-class USB stack. The bulk engine is synchronous/polled, uses one reusable QH/qTD and an identity-addressed below-4-GiB DMA assumption, and only supports one 4096-byte-or-smaller transfer at a time. This phase does not implement EHCI interrupts, a timer service, downstream hubs, HID keyboard/mouse, USB 1.x companion host drivers, hot-plug, suspend/resume, a general block-device registry/VFS integration, xHCI, or high-capacity READ CAPACITY(16). Test in QEMU first; do not use the WRITE(10) API against important media until block I/O, error recovery and device tests are mature.
