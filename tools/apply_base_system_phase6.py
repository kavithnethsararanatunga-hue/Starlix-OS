#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime
import sys

root = Path(__file__).resolve().parents[1]
build = root / "scripts/build.sh"
src = build.read_text(encoding="utf-8")
needle = 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_file_reader.cpp" -o "$STBMGR_DIR/build/apfs_file_reader.o"\n'
replacement = needle + 'clang++ $CXXFLAGS -c "$STBMGR_DIR/src/base_system_reader.cpp" -o "$STBMGR_DIR/build/base_system_reader.o"\n'
link_old = '"$STBMGR_DIR/build/apfs_file_reader.o" "$COMMON_OBJDIR/runtime.o"'
link_new = '"$STBMGR_DIR/build/apfs_file_reader.o" "$STBMGR_DIR/build/base_system_reader.o" "$COMMON_OBJDIR/runtime.o"'

if src.count('base_system_reader.cpp') == 1 and src.count('base_system_reader.o') == 2:
    print("Phase 6 build integration already present; no change needed.")
    raise SystemExit(0)
if src.count(needle) != 1:
    raise SystemExit("ABORT: expected APFS file-reader compile line not found exactly once.")
if src.count(link_old) != 1:
    raise SystemExit("ABORT: expected stbootmngr link object sequence not found exactly once.")

backup = build.with_name(build.name + ".bak-base-system-phase6-" + datetime.now().strftime("%Y%m%d_%H%M%S"))
backup.write_bytes(build.read_bytes())
src = src.replace(needle, replacement, 1).replace(link_old, link_new, 1)
build.write_text(src, encoding="utf-8")
print("Updated:", build.relative_to(root))
print("Backup :", backup.relative_to(root))
print("Added composed RXFS -> UDIF -> APFS -> boot.efi byte reader to stbootmngr build; main.cpp unchanged.")
