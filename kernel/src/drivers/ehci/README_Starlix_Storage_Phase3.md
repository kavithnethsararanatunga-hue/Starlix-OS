# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.

# Starlix OS 1.0.Beta — Storage Phase 3

Phase 3 connects the existing Phase 2 EHCI USB Mass Storage read API to a small read-only storage layer.

## Included

- Generic bounded block reads and USB mass-storage device query APIs.
- MBR primary partition discovery with address bounds checking.
- GPT header and entry-array CRC32 validation.
- Read-only FAT32 mount, root-directory listing, and root-level 8.3 filename reads.
- Whole-disk FAT32 (superfloppy) support for test media.
- Automatic boot-time scan following a successful EHCI initialization.
- Optional `HELLO.TXT` read smoke test for the provided QEMU disk image.
- Timestamped backups and rollback on build/package failure.
- Host-side synthetic tests for FAT32 file reading, MBR scanning, and GPT CRC validation.

## Important limitations

This is not yet a VFS mount implementation or a complete filesystem. It is read-only, supports FAT32 short names only in the root directory, does not support writes or nested paths, and does not parse MBR extended/logical partitions. GPT scanning is limited to 256 entries. USB operations still rely on the Phase 2 synchronous polling EHCI/BOT implementation and its 4096-byte request limit; interrupts, hubs, hot-plug, and READ CAPACITY(16) are not included.

## Install on Ubuntu

Download the ZIP to `~/Downloads`, then:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
cd ~/Downloads && unzip -o Starlix_Storage_Phase3_Patch.zip -d Starlix_Storage_Phase3_Patch
bash ~/Downloads/Starlix_Storage_Phase3_Patch/install_storage_phase3.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

For kernel build only (no EFI/SSP/disk-image repackaging), use `--kernel-only` in place of `--image`.

Backup path pattern:

`/home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta/.backups/storage-phase3-<timestamp>/`

## Runtime test

After a successful installation/package build:

```bash
bash ~/Downloads/Starlix_Storage_Phase3_Patch/test_ehci_phase3_qemu.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta
```

This creates a **temporary 64 MiB FAT32 disk image**, puts `HELLO.TXT` on it, and attaches it only to QEMU. It does not access a physical USB drive. The serial log is saved to `image/ehci-phase3-qemu.log`.

The expected messages are examples, not pre-confirmed runtime output. The actual project build and QEMU test must run on the user's machine.
