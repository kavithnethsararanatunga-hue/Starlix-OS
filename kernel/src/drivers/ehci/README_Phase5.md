# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.

# Starlix OS 1.0.Beta — Storage Phase 5: partitioned FAT32 runtime validation

Phase 4 QEMU test passed for a FAT32 superfloppy. Phase 5 addresses the next gap by testing the same files inside an MBR primary partition whose FAT32 volume begins at LBA 2048.

## Changes

- Existing MBR/GPT partition scanning remains in the storage layer.
- Boot-time partition mounting now attempts read-only smoke reads for `HELLO.TXT`, `Long File Name.txt`, and `Docs/Nested Note.txt`, then lists `Docs`. Previously the smoke reads were only in the superfloppy branch.
- Host-side synthetic tests now include a FAT32 filesystem at LBA 2048 inside an MBR type `0x0C` partition and check automatic mount plus nested-file reads.
- The QEMU script creates a disposable 64 MiB MBR-partitioned disk image; it does not attach or modify a physical USB drive.

FAT32 remains read-only. Full filesystem write support, general VFS namespace, USB hubs, interrupts, hot-plug, full Unicode case folding, and physical USB hardware validation remain future work. This is not yet a production-ready or Windows-class storage stack.

## Install

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
cd ~/Downloads && unzip -o Starlix_Storage_Phase5_Patch.zip -d Starlix_Storage_Phase5_Patch
bash ~/Downloads/Starlix_Storage_Phase5_Patch/install_storage_phase5.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

The installer runs host tests before changing the project, takes a timestamped backup of storage sources, kernel build outputs, and packaging artifacts, builds the kernel, and repackages ECT/EFI/SSP/disk image. It rolls back if build or packaging fails. `--kernel-only` skips packaging.

## QEMU runtime test

```bash
bash ~/Downloads/Starlix_Storage_Phase5_Patch/test_ehci_phase5_qemu.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta
```

Requires QEMU, matching OVMF firmware, `dosfstools`, `mtools`, and Python 3. The script creates a disposable 64 MiB MBR disk with one FAT32 partition starting at LBA 2048, copies short-name, long-name, and nested-directory files into it, and attaches it to the QEMU EHCI bus. Serial output is saved to `image/ehci-phase5-qemu.log`. Close QEMU to finish.

Expected lines if it works:

```text
[STORAGE] Valid partitions found: 1
[FAT32] Mounted partition 1 at LBA 2048.
[FAT32] HELLO.TXT read smoke test passed
[FAT32] Long File Name.txt read smoke test passed
[FAT32] Docs/Nested Note.txt read smoke test passed
[OK] SYSTEM BOOT COMPLETE - ENTERING IDLE
```

These expected messages are not a success claim until they appear in the actual QEMU log on your machine.
