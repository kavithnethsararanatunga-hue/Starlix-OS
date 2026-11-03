#!/usr/bin/env python3
"""Integrate the RXFS -> BaseSystem -> Starlix boot.efi chainload path."""
from datetime import datetime
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
PAYLOAD = ROOT / "tools/phase8_payload/stbtmngr_main.cpp"

targets = {
    "main": ROOT / "boot/x64/stbootmngr.efi/src/main.cpp",
    "build": ROOT / "scripts/build.sh",
    "test_script": ROOT / "tests/test_stbootmngr_basesystem.sh",
    "test_source": ROOT / "tests/stbootmngr_basesystem_selftest.cpp",
}

if not PAYLOAD.is_file():
    raise SystemExit(f"ABORT: required payload missing: {PAYLOAD}")

contents = {key: path.read_text(encoding="utf-8") for key, path in targets.items()}

# Idempotent success only if every expected Phase 8 integration is present.
if ("TryRxfsBootEfi" in contents["main"] and
        "stboot_handoff.o" in contents["build"] and
        "stboot_handoff.cpp" in contents["test_script"] and
        "stboot_handoff.hpp" in contents["test_source"]):
    print("Phase 8 already integrated; no files changed.")
    raise SystemExit(0)

if "TryRxfsBootEfi" in contents["main"]:
    raise SystemExit("ABORT: main.cpp looks partially integrated; inspect manually.")
if "SspMount(BS, &ssp)" not in contents["main"]:
    raise SystemExit("ABORT: unexpected main.cpp; original SSP entry path not found.")
if "stboot_handoff.o" in contents["build"]:
    raise SystemExit("ABORT: build.sh is partially integrated; inspect manually.")

build_compile_anchor = (
    'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/base_system_reader.cpp" '
    '-o "$STBMGR_DIR/build/base_system_reader.o"\n'
)
build_link_anchor = '"$STBMGR_DIR/build/main.o" "$STBMGR_DIR/build/ssp.o"'
if contents["build"].count(build_compile_anchor) != 1 or contents["build"].count(build_link_anchor) != 1:
    raise SystemExit("ABORT: build.sh Phase 6 anchors are not unique; no changes made.")

test_anchor = '  "$ROOT/boot/x64/stbootmngr.efi/src/base_system_reader.cpp" \\\n'
if contents["test_script"].count(test_anchor) != 1:
    raise SystemExit("ABORT: BaseSystem test build anchor is not unique; no changes made.")
include_anchor = '#include "boot/x64/stbootmngr.efi/src/base_system_reader.hpp"\n'
if contents["test_source"].count(include_anchor) != 1:
    raise SystemExit("ABORT: BaseSystem selftest include anchor is not unique; no changes made.")
if "StbootCreateFsHandoff" in contents["test_source"]:
    raise SystemExit("ABORT: selftest appears partially integrated; inspect manually.")

new_build = contents["build"].replace(
    build_compile_anchor,
    build_compile_anchor +
    'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/stboot_handoff.cpp" -o "$STBMGR_DIR/build/stboot_handoff.o"\n',
    1,
).replace(
    build_link_anchor,
    '"$STBMGR_DIR/build/main.o" "$STBMGR_DIR/build/stboot_handoff.o" "$STBMGR_DIR/build/ssp.o"',
    1,
)
new_test_script = contents["test_script"].replace(
    test_anchor,
    test_anchor + '  "$ROOT/boot/x64/stbootmngr.efi/src/stboot_handoff.cpp" \\\n',
    1,
)
new_test_source = contents["test_source"].replace(
    include_anchor,
    include_anchor + '#include "boot/x64/stbootmngr.efi/src/stboot_handoff.hpp"\n',
    1,
)
selftest_anchor = '''    if (!system.Opened || system.BootEfiPath.FileId != 13 || system.BootEfiPath.IsDirectory) return 4;

    APFS_FILE_INFO info{};'''
selftest_added = '''    if (!system.Opened || system.BootEfiPath.FileId != 13 || system.BootEfiPath.IsDirectory) return 4;

    STBOOT_FS_HANDOFF handoff{};
    if (StbootCreateFsHandoff(&system, &handoff) != EFI_SUCCESS) return 12;
    const uint8_t expectedSignature[8] = {'S','T','B','F','S','0','0','1'};
    for (uint32_t i = 0; i < 8; ++i)
        if (handoff.Signature[i] != expectedSignature[i]) return 13;
    if (handoff.Version != STBOOT_FS_HANDOFF_VERSION ||
        handoff.Size != sizeof(STBOOT_FS_HANDOFF) || !handoff.Context ||
        !handoff.Lookup || !handoff.GetInfo || !handoff.Read ||
        !(handoff.Flags & STBOOT_FS_HANDOFF_READ_ONLY) ||
        !(handoff.Flags & STBOOT_FS_HANDOFF_BOOT_SERVICES_REQUIRED)) return 14;

    uint64_t handoffFileId = 0;
    uint16_t handoffDirFlags = 0;
    BOOLEAN handoffIsDirectory = TRUE;
    if (handoff.Lookup(handoff.Context, "/System/Library/CoreServices/boot.efi",
                       &handoffFileId, &handoffDirFlags, &handoffIsDirectory) != EFI_SUCCESS ||
        handoffFileId != 13 || handoffIsDirectory != FALSE) return 15;

    STBOOT_FS_FILE_INFO handoffInfo{};
    if (handoff.GetInfo(handoff.Context, handoffFileId, &handoffInfo) != EFI_SUCCESS ||
        handoffInfo.FileId != 13 || handoffInfo.Size != 4100 ||
        (handoffInfo.Mode & 0xF000U) != 0x8000U) return 16;

    uint8_t handoffOut[16]{};
    UINTN handoffRead = 0;
    if (handoff.Read(handoff.Context, handoffFileId, 4096, sizeof(handoffOut),
                     handoffOut, &handoffRead) != EFI_SUCCESS || handoffRead != 4 ||
        handoffOut[0] != 'T' || handoffOut[1] != 'A' ||
        handoffOut[2] != 'I' || handoffOut[3] != 'L') return 17;

    APFS_FILE_INFO info{};'''
if new_test_source.count(selftest_anchor) != 1:
    raise SystemExit("ABORT: BaseSystem selftest insertion anchor is not unique; no changes made.")
new_test_source = new_test_source.replace(selftest_anchor, selftest_added, 1)

changes = {
    targets["main"]: PAYLOAD.read_text(encoding="utf-8"),
    targets["build"]: new_build,
    targets["test_script"]: new_test_script,
    targets["test_source"]: new_test_source,
}
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backups = []
for path, new_data in changes.items():
    if path.read_text(encoding="utf-8") == new_data:
        continue
    backup = path.with_name(path.name + f".bak-stbootmngr-phase8-{stamp}")
    if backup.exists():
        raise SystemExit(f"ABORT: backup already exists: {backup}")
    backups.append((path, backup, new_data))

for path, backup, new_data in backups:
    backup.write_bytes(path.read_bytes())
    path.write_text(new_data, encoding="utf-8")
    print(f"Backup : {backup.relative_to(ROOT)}")
    print(f"Updated: {path.relative_to(ROOT)}")

print("Phase 8 integrated: RXFS-first BaseSystem boot.efi chainload and versioned read-only file API handoff.")
print("Legacy SSP -> Rxload.efi remains a fallback only if RXFS/APFS cannot prepare or load boot.efi.")
print("No physical disk access or write operation was added; no ExitBootServices call was added.")
