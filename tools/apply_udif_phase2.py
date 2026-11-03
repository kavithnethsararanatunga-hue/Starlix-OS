#!/usr/bin/env python3
"""Integrate the read-only UDIF/APFS partition mapper into the stbootmngr build."""
from datetime import datetime
from pathlib import Path
import sys

root = Path.cwd()
script = root / "scripts" / "build.sh"
required = [
    root / "boot/x64/stbootmngr.efi/src/udif_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/udif_reader.cpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_reader.hpp",
    root / "boot/x64/stbootmngr.efi/src/apfs_reader.cpp",
]
missing = [str(p.relative_to(root)) for p in required if not p.is_file()]
if missing:
    sys.exit("ABORT: missing files:\n  " + "\n  ".join(missing))
if not script.is_file():
    sys.exit("ABORT: scripts/build.sh not found; run from project root")

src = script.read_text(encoding="utf-8")
compile_anchor = 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_reader.cpp" -o "$STBMGR_DIR/build/apfs_reader.o"'
source_marker = '"$STBMGR_DIR/src/udif_reader.cpp"'
object_marker = '"$STBMGR_DIR/build/udif_reader.o"'
if source_marker in src or object_marker in src:
    if source_marker in src and object_marker in src:
        print("UDIF Phase 2 build integration already present; no changes made.")
        raise SystemExit(0)
    sys.exit("ABORT: partial UDIF build integration detected; no build file changed")
if src.count(compile_anchor) != 1:
    sys.exit("ABORT: expected apfs_reader compile line not found exactly once; no build file changed")

compile_line = '\nclang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_reader.cpp" -o "$STBMGR_DIR/build/udif_reader.o"'
updated = src.replace(compile_anchor, compile_anchor + compile_line, 1)
start = updated.index('echo "[*] stbtmngr.efi"')
link_start = updated.index("lld-link", start)
link_end = updated.index('-out:"$STBMGR_DIR/build/stbtmngr.efi"', link_start)
block = updated[link_start:link_end]
apfs_obj = '"$STBMGR_DIR/build/apfs_reader.o"'
udif_obj = '"$STBMGR_DIR/build/udif_reader.o"'
if apfs_obj not in block:
    sys.exit("ABORT: APFS object missing from stbtmngr linker block; no build file changed")
if udif_obj not in block:
    block = block.replace(apfs_obj, apfs_obj + " " + udif_obj, 1)
    updated = updated[:link_start] + block + updated[link_end:]

if updated == src:
    print("No build changes needed.")
    raise SystemExit(0)
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = script.with_name(script.name + ".bak-udif-phase2-" + stamp)
backup.write_bytes(script.read_bytes())
script.write_text(updated, encoding="utf-8")
print("Updated:", script.relative_to(root))
print("Backup :", backup.relative_to(root))
print("Added UDIF object compile/link to stbootmngr only; main.cpp control flow remains unchanged.")
