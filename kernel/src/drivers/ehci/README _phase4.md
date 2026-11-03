# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.

# Starlix OS 1.0.Beta — Storage Phase 4

Phase 4 builds on the successful Phase 3.1 QEMU test (USB EHCI/MSC BOT, block registry, FAT32 mount, `HELLO.TXT` read) and adds:

- VFAT long-file-name sequence validation, including the short-name checksum.
- UTF-16 LFN code-unit decoding into UTF-8 output (including surrogate pairs).
- Root directory listings that display a validated long name instead of only the 8.3 alias.
- Relative nested-path lookup with `/` or `\\` separators, e.g. `Docs/Nested Note.txt`.
- `fat32_read_file()` and `fat32_list_directory()` read-only APIs.
- Boot-time smoke checks for `Long File Name.txt`, `Docs/Nested Note.txt`, and listing `Docs`, when present on a test superfloppy.
- A QEMU test image populated with a short-name file, a long-name file, and a file under `Docs/`.
- Timestamped backup and rollback of Storage Phase 3 files, kernel build products, and disk/EFI/SSP artifacts if a project build/package step fails.

The filesystem remains read-only. File-name comparison folds ASCII letters case-insensitively; non-ASCII UTF-8 byte sequences are compared exactly (full Unicode case folding is not implemented). Path traversal with `.` and `..`, FAT writes, delete/create, permissions, timestamps, journaling, USB hubs, interrupts, hot-plug, and physical USB hardware validation remain future work. This prototype should not yet be used as the system root.

## Install

First confirm Storage Phase 3.1 is installed; the installer checks for `#include "storage/storage.cpp"` in `kernel/src/main.cpp`, the existing compatibility bridge used by this version of the build scripts.

Download and extract this ZIP in `~/Downloads`, then run:

```bash
# COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.
cd ~/Downloads && unzip -o Starlix_Storage_Phase4_Patch.zip -d Starlix_Storage_Phase4_Patch
bash ~/Downloads/Starlix_Storage_Phase4_Patch/install_storage_phase4.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta --image
```

Before changing your project, the installer runs the synthetic in-memory host tests with `-Wall -Wextra -Werror`. It then backs up `kernel/src/storage`, `kernel/build`, and existing ECT/EFI/SSP/disk artifacts to `.backups/storage-phase4-<timestamp>/`. It builds the freestanding kernel, then rebuilds ECT, EFI stages, SSP, and the disk image. If build or packaging fails, it restores the Storage Phase 3 source/build/artifacts. `--kernel-only` skips EFI/SSP/image packaging.

## Host-side tests

The host tests use a synthetic in-memory FAT32 volume and fake EHCI read callbacks; they do not read or write a physical drive:

```bash
cd ~/Downloads/Starlix_Storage_Phase4_Patch
clang++ -std=c++26 -O2 -Wall -Wextra -Werror \
  -I tests/stubs/storage -I tests/stubs -I payload/kernel/src \
  payload/kernel/src/storage/storage.cpp tests/storage_host_test.cpp \
  -o /tmp/starlix-storage-phase4-test
/tmp/starlix-storage-phase4-test
```

Expected output includes `PASS: VFAT long filename read.`, `PASS: FAT32 nested directory read/list.`, plus earlier FAT32, registry, MBR, and GPT checks. These host tests are separate from the user's kernel integration and QEMU runtime test.

## QEMU runtime test

```bash
bash ~/Downloads/Starlix_Storage_Phase4_Patch/test_ehci_phase4_qemu.sh \
  /home/kavith/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta
```

The script creates a disposable 64 MiB FAT32 image with `HELLO.TXT`, `Long File Name.txt`, and `Docs/Nested Note.txt`, attaches it to QEMU's virtual EHCI bus, and writes serial output to `image/ehci-phase4-qemu.log`. No physical USB drive is used. Close the QEMU window when the boot output is complete.

Expected runtime lines if the integration is working:

```text
[FAT32] root entry: Long File Name.txt (... bytes)
[FAT32] Long File Name.txt read smoke test passed; bytes read: ...
[FAT32] Docs/Nested Note.txt read smoke test passed; bytes read: ...
[FAT32] directory entry: Nested Note.txt (... bytes)
[OK] SYSTEM BOOT COMPLETE - ENTERING IDLE
```

Treat these messages as passed only after they appear in the actual log generated on your machine.
