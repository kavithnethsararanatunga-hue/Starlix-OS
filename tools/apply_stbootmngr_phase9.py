#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "boot/x64/stbootmngr.efi/src/main.cpp"
if not TARGET.is_file():
    raise SystemExit(f"ABORT: not found: {TARGET}")
src = TARGET.read_text(encoding="utf-8")
if "Starlix boot manager: RXFS-first Phase 9" in src:
    print("Phase 9 diagnostics already integrated; no changes made.")
    raise SystemExit(0)

replacements = [
("constexpr UINTN kBootEfiReadChunk = 64ULL * 1024ULL;\n",
 "constexpr UINTN kBootEfiReadChunk = 64ULL * 1024ULL;\n\n"
 "// Phase 9: firmware text-console diagnostics for isolated RXFS disk tests.\n"
 "static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* gStbootConsole = nullptr;\n"
 "static void StbootLog(const CHAR16* Message) {\n"
 "    if (!gStbootConsole || !gStbootConsole->OutputString || !Message) return;\n"
 "    (void)gStbootConsole->OutputString(gStbootConsole, const_cast<CHAR16*>(Message));\n"
 "}\n"),
("    EFI_STATUS status = RxfsMount(BS, &rxfs);\n"
 "    if (status != EFI_SUCCESS) return status;\n\n"
 "    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n"
 "    if (status != EFI_SUCCESS) return status;\n\n"
 "    APFS_FILE_INFO bootInfo{};\n"
 "    status = StbootGetBootEfiInfo(&baseSystem, &bootInfo);\n"
 "    if (status != EFI_SUCCESS) return status;",
 "    StbootLog(u\"[RXFS] Scanning UEFI disk handles...\\r\\n\");\n"
 "    EFI_STATUS status = RxfsMount(BS, &rxfs);\n"
 "    if (status != EFI_SUCCESS) {\n"
 "        StbootLog(u\"[RXFS] Mount failed; trying legacy SSP fallback.\\r\\n\");\n"
 "        return status;\n"
 "    }\n"
 "    StbootLog(u\"[RXFS] Volume mounted read-only.\\r\\n\");\n\n"
 "    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n"
 "    if (status != EFI_SUCCESS) {\n"
 "        StbootLog(u\"[BaseSystem] Open failed; trying legacy SSP fallback.\\r\\n\");\n"
 "        return status;\n"
 "    }\n"
 "    StbootLog(u\"[BaseSystem] APFS volume/path opened.\\r\\n\");\n\n"
 "    APFS_FILE_INFO bootInfo{};\n"
 "    status = StbootGetBootEfiInfo(&baseSystem, &bootInfo);\n"
 "    if (status != EFI_SUCCESS) {\n"
 "        StbootLog(u\"[APFS] boot.efi metadata lookup failed.\\r\\n\");\n"
 "        return status;\n"
 "    }"),
("    status = StbootCreateFsHandoff(&baseSystem, &handoff);\n"
 "    if (status != EFI_SUCCESS) return status;",
 "    status = StbootCreateFsHandoff(&baseSystem, &handoff);\n"
 "    if (status != EFI_SUCCESS) {\n"
 "        StbootLog(u\"[Handoff] Filesystem handoff creation failed.\\r\\n\");\n"
 "        return status;\n"
 "    }"),
("    if (status != EFI_SUCCESS || !childImage)\n"
 "        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;",
 "    if (status != EFI_SUCCESS || !childImage) {\n"
 "        StbootLog(u\"[UEFI] LoadImage(boot.efi) failed.\\r\\n\");\n"
 "        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;\n"
 "    }"),
("    childLoadedImage->LoadOptionsSize = static_cast<uint32_t>(sizeof(handoff));\n"
 "    childLoadedImage->LoadOptions = &handoff;\n\n"
 "    (void)BS->StartImage(childImage, nullptr, nullptr);",
 "    childLoadedImage->LoadOptionsSize = static_cast<uint32_t>(sizeof(handoff));\n"
 "    childLoadedImage->LoadOptions = &handoff;\n"
 "    StbootLog(u\"[RXFS] Starting boot.efi with read-only filesystem handoff.\\r\\n\");\n\n"
 "    (void)BS->StartImage(childImage, nullptr, nullptr);"),
("static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n"
 "    SSP_CONTEXT ssp;",
 "static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n"
 "    StbootLog(u\"[BOOT] Using transitional legacy SSP fallback.\\r\\n\");\n"
 "    SSP_CONTEXT ssp;"),
("    EFI_BOOT_SERVICES* BS = SystemTable->BootServices;\n\n"
 "    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);",
 "    EFI_BOOT_SERVICES* BS = SystemTable->BootServices;\n"
 "    gStbootConsole = SystemTable->ConOut;\n"
 "    StbootLog(u\"\\r\\nStarlix boot manager: RXFS-first Phase 9\\r\\n\");\n\n"
 "    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);"),
]
for old, new in replacements:
    count = src.count(old)
    if count != 1:
        raise SystemExit(f"ABORT: expected replacement anchor exactly once, found {count}: {old[:100]!r}")
    src = src.replace(old, new, 1)

backup = TARGET.with_name(TARGET.name + ".bak-phase9-diagnostics-" + datetime.now().strftime("%Y%m%d_%H%M%S"))
backup.write_bytes(TARGET.read_bytes())
TARGET.write_text(src, encoding="utf-8", newline="\n")
print(f"Backup : {backup.relative_to(ROOT)}")
print(f"Updated: {TARGET.relative_to(ROOT)}")
print("Added console diagnostics for RXFS mount, BaseSystem/APFS lookup, boot.efi load, and legacy fallback.")
print("No block-device writes, no partition writes, and no ExitBootServices changes were added.")
