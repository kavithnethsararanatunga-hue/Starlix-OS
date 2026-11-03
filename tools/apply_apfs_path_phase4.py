#!/usr/bin/env python3
"""Safely integrate APFS DREC/path lookup into the stbootmngr EFI build."""
from datetime import datetime
from pathlib import Path
import sys

root = Path.cwd()
script = root / "scripts" / "build.sh"
required = [
    root / "boot/x64/stbootmngr.efi/src/apfs_path_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_path_reader.cpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_metadata_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_metadata_reader.cpp",
]
missing = [str(p.relative_to(root)) for p in required if not p.is_file()]
if missing:
    sys.exit("ABORT: missing required files:\n  " + "\n  ".join(missing))
if not script.is_file():
    sys.exit("ABORT: scripts/build.sh not found; run this from the project root")

src = script.read_text(encoding="utf-8")
source_mark = '"$STBMGR_DIR/src/apfs_path_reader.cpp"'
object_mark = '"$STBMGR_DIR/build/apfs_path_reader.o"'
if source_mark in src or object_mark in src:
    if source_mark in src and object_mark in src:
        print("APFS path Phase 4 build integration already present; no changes made.")
        raise SystemExit(0)
    sys.exit("ABORT: partial APFS path integration detected; build file unchanged")
anchor = 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_metadata_reader.cpp" -o "$STBMGR_DIR/build/apfs_metadata_reader.o"'
if src.count(anchor) != 1:
    sys.exit("ABORT: expected APFS metadata compile line not found exactly once; build file unchanged")
updated = src.replace(anchor, anchor + '\nclang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_path_reader.cpp" -o "$STBMGR_DIR/build/apfs_path_reader.o"', 1)
start = updated.index('echo "[*] stbtmngr.efi"')
link_start = updated.index("lld-link", start)
link_end = updated.index('-out:"$STBMGR_DIR/build/stbtmngr.efi"', link_start)
block = updated[link_start:link_end]
metadata_obj = '"$STBMGR_DIR/build/apfs_metadata_reader.o"'
if metadata_obj not in block:
    sys.exit("ABORT: APFS metadata object absent from stbtmngr link block; build file unchanged")
block = block.replace(metadata_obj, metadata_obj + ' ' + object_mark, 1)
updated = updated[:link_start] + block + updated[link_end:]
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = script.with_name(script.name + ".bak-apfs-path-phase4-" + stamp)
backup.write_bytes(script.read_bytes())
script.write_text(updated, encoding="utf-8")
print("Updated:", script.relative_to(root))
print("Backup :", backup.relative_to(root))
print("Added read-only APFS directory/path resolver to stbootmngr build; boot flow unchanged.")
