#!/usr/bin/env python3
"""Safely integrate the RXFS/APFS reader phase-1 objects into scripts/build.sh.
Run from the Starlix project root after unpacking the patch files."""
from datetime import datetime
from pathlib import Path
import sys

root = Path.cwd()
build = root / "scripts" / "build.sh"
required = [
    root / "boot/x64/stbootmngr.efi/src/rxfs_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/rxfs_reader.cpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_reader.cpp",
    root / "rxfs/include/rxfs_format.hpp",
]
missing = [str(p.relative_to(root)) for p in required if not p.is_file()]
if missing:
    sys.exit("ABORT: missing required files:\n  " + "\n  ".join(missing))
if not build.is_file():
    sys.exit("ABORT: scripts/build.sh not found; run this script from the Starlix project root.")

src = build.read_text(encoding="utf-8")
compile_anchor = 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/ssp.cpp" -o "$STBMGR_DIR/build/ssp.o"'
if src.count(compile_anchor) != 1:
    # Idempotent rerun is allowed if both objects have already been integrated.
    if '"$STBMGR_DIR/src/rxfs_reader.cpp"' not in src or '"$STBMGR_DIR/src/apfs_reader.cpp"' not in src:
        sys.exit("ABORT: expected stbootmngr SSP compile line was not found exactly once; no build file changed.")

start_marker = 'echo "[*] stbtmngr.efi"'
link_marker = '-out:"$STBMGR_DIR/build/stbtmngr.efi"'
if start_marker not in src or link_marker not in src:
    sys.exit("ABORT: could not uniquely locate the stbootmngr build block; no build file changed.")

updated = src
compile_lines = (
    '\n# Read-only RXFS/APFS image inspection modules. The main boot path is not switched yet.\n'
    'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/rxfs_reader.cpp" -o "$STBMGR_DIR/build/rxfs_reader.o"\n'
    'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_reader.cpp" -o "$STBMGR_DIR/build/apfs_reader.o"'
)
if '"$STBMGR_DIR/src/rxfs_reader.cpp"' not in updated:
    updated = updated.replace(compile_anchor, compile_anchor + compile_lines, 1)

stb_start = updated.index(start_marker)
link_start = updated.index("lld-link", stb_start)
link_end = updated.index(link_marker, link_start)
link_block = updated[link_start:link_end]
rxobj = '"$STBMGR_DIR/build/rxfs_reader.o"'
apobj = '"$STBMGR_DIR/build/apfs_reader.o"'
if rxobj not in link_block or apobj not in link_block:
    ssp_obj = '"$STBMGR_DIR/build/ssp.o"'
    if ssp_obj not in link_block:
        sys.exit("ABORT: stbootmngr linker block did not contain its SSP object; no build file changed.")
    link_block = link_block.replace(ssp_obj, ssp_obj + " " + rxobj + " " + apobj, 1)
    updated = updated[:link_start] + link_block + updated[link_end:]

if updated == src:
    print("RXFS/APFS phase-1 build integration is already present; no changes made.")
    raise SystemExit(0)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = build.with_name(build.name + ".bak-rxfs-apfs-phase1-" + stamp)
backup.write_bytes(build.read_bytes())
build.write_text(updated, encoding="utf-8")
print("Updated:", build.relative_to(root))
print("Backup :", backup.relative_to(root))
print("Added reader object compilation/linking only; main.cpp and physical disks were not changed.")
