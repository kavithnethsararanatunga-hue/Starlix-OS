#!/usr/bin/env python3
"""Add Phase 11 test-child result logging and guard the fixture test main."""
from pathlib import Path
from datetime import datetime
import sys

root = Path(__file__).resolve().parents[1]
main = root / "boot/x64/stbootmngr.efi/src/main.cpp"
fixture = root / "tests/stbootmngr_basesystem_selftest.cpp"
main_src = main.read_text(encoding="utf-8")
fixture_src = fixture.read_text(encoding="utf-8")
anchor = r'''    StbootSerialLog(BS, "[STBOOT] Starting Starlix boot.efi via StartImage\r\n");
    (void)BS->StartImage(childImage, nullptr, nullptr);
    StbootSerialLog(BS, "[STBOOT] boot.efi returned; halting instead of SSP fallback\r\n");'''
replacement = r'''    StbootSerialLog(BS, "[STBOOT] Starting Starlix boot.efi via StartImage\r\n");
    const EFI_STATUS childStatus = BS->StartImage(childImage, nullptr, nullptr);
    StbootSerialLog(BS, childStatus == EFI_SUCCESS
        ? "[STBOOT] child returned EFI_SUCCESS (Phase 11 handoff test passed)\r\n"
        : "[STBOOT] child returned EFI error (Phase 11 handoff test failed)\r\n");
    StbootSerialLog(BS, "[STBOOT] boot.efi returned; halting instead of SSP fallback\r\n");'''
if main_src.count(anchor) != 1:
    print(f"ABORT: expected StartImage anchor exactly once; found {main_src.count(anchor)}", file=sys.stderr)
    sys.exit(1)
if "Phase 11 handoff test passed" in main_src:
    print("ABORT: Phase 11 StartImage status logging already appears present", file=sys.stderr)
    sys.exit(1)
start = "\nint main() {\n    BuildFileImage();"
if fixture_src.count(start) != 1:
    print(f"ABORT: expected basesystem self-test main anchor exactly once; found {fixture_src.count(start)}", file=sys.stderr)
    sys.exit(1)
if "STBOOT_PHASE11_EXPORT_ONLY" in fixture_src:
    print("ABORT: Phase 11 fixture-main guard already present", file=sys.stderr)
    sys.exit(1)
if not fixture_src.rstrip().endswith("}"):
    print("ABORT: unexpected end of basesystem self-test source", file=sys.stderr)
    sys.exit(1)

# Validate all anchors before writing either file; create backups of both first.
stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
main_backup = main.with_name(main.name + ".bak-phase11-child-result-" + stamp)
fixture_backup = fixture.with_name(fixture.name + ".bak-phase11-export-" + stamp)
main_backup.write_bytes(main.read_bytes())
fixture_backup.write_bytes(fixture.read_bytes())
main.write_text(main_src.replace(anchor, replacement, 1), encoding="utf-8")
fixture_new = fixture_src.replace(
    start,
    "\n#ifndef STBOOT_PHASE11_EXPORT_ONLY\nint main() {\n    BuildFileImage();",
    1,
).rstrip() + "\n#endif // STBOOT_PHASE11_EXPORT_ONLY\n"
fixture.write_text(fixture_new, encoding="utf-8")
print("Backup :", main_backup.relative_to(root))
print("Backup :", fixture_backup.relative_to(root))
print("Updated:", main.relative_to(root))
print("Updated:", fixture.relative_to(root))
print("StartImage return status logging enabled; fixture main can be omitted for export. No ExitBootServices or disk writes added.")
