#!/usr/bin/env python3
"""Safely integrate the APFS object-map / volume metadata reader into stbootmngr."""
from datetime import datetime
from pathlib import Path
import sys

root = Path.cwd()
script = root / "scripts" / "build.sh"
required = [
    root / "boot/x64/stbootmngr.efi/src/apfs_metadata_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_metadata_reader.cpp",
    root / "boot/x64/stbootmngr.efi/src/udif_reader.cpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_reader.cpp",
]
missing = [str(p.relative_to(root)) for p in required if not p.is_file()]
if missing:
    sys.exit("ABORT: missing required files:\n  " + "\n  ".join(missing))
if not script.is_file():
    sys.exit("ABORT: scripts/build.sh not found; run from project root")

src = script.read_text(encoding="utf-8")
source_mark = '"$STBMGR_DIR/src/apfs_metadata_reader.cpp"'
object_mark = '"$STBMGR_DIR/build/apfs_metadata_reader.o"'
if source_mark in src or object_mark in src:
    if source_mark in src and object_mark in src:
        print("APFS metadata Phase 3 build integration already present; no changes made.")
        raise SystemExit(0)
    sys.exit("ABORT: partial APFS metadata integration detected; build file unchanged")
anchor = 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_reader.cpp" -o "$STBMGR_DIR/build/udif_reader.o"'
if src.count(anchor) != 1:
    sys.exit("ABORT: expected UDIF compile line not found exactly once; build file unchanged")
updated = src.replace(anchor, anchor + '\nclang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_metadata_reader.cpp" -o "$STBMGR_DIR/build/apfs_metadata_reader.o"', 1)
start = updated.index('echo "[*] stbtmngr.efi"')
link_start = updated.index("lld-link", start)
link_end = updated.index('-out:"$STBMGR_DIR/build/stbtmngr.efi"', link_start)
block = updated[link_start:link_end]
if '"$STBMGR_DIR/build/udif_reader.o"' not in block:
    sys.exit("ABORT: UDIF object absent from linker block; build file unchanged")
block = block.replace('"$STBMGR_DIR/build/udif_reader.o"', '"$STBMGR_DIR/build/udif_reader.o" ' + object_mark, 1)
updated = updated[:link_start] + block + updated[link_end:]
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = script.with_name(script.name + ".bak-apfs-metadata-phase3-" + stamp)
backup.write_bytes(script.read_bytes())
script.write_text(updated, encoding="utf-8")
print("Updated:", script.relative_to(root))
print("Backup :", backup.relative_to(root))
print("Added read-only APFS metadata resolver to stbootmngr build; main.cpp control flow is unchanged.")
