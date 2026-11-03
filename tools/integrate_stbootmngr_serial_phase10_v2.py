#!/usr/bin/env python3
"""Integrate Phase 10 UEFI Serial I/O diagnostics into the Phase 9 boot manager."""
from pathlib import Path
from datetime import datetime
import sys

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "boot/x64/stbootmngr.efi/src/main.cpp"
if not MAIN.is_file():
    raise SystemExit(f"ABORT: source file not found: {MAIN}")
src = MAIN.read_text(encoding="utf-8")
if "STBOOT_SERIAL_IO_PROTOCOL" in src:
    print("Phase 10 serial diagnostics already integrated; no changes made.")
    raise SystemExit(0)

helper = r'''// Optional UEFI Serial I/O diagnostics. If firmware does not expose the
// protocol, logging is unavailable but the boot flow continues unchanged.
struct STBOOT_SERIAL_IO_PROTOCOL;
using STBOOT_SERIAL_FN_RESET = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*);
using STBOOT_SERIAL_FN_SET_ATTRIBUTES = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint64_t, uint32_t, uint32_t, uint32_t, uint8_t, uint8_t);
using STBOOT_SERIAL_FN_SET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t);
using STBOOT_SERIAL_FN_GET_CONTROL = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, uint32_t*);
using STBOOT_SERIAL_FN_TRANSFER = EFI_STATUS (ABI *)(STBOOT_SERIAL_IO_PROTOCOL*, UINTN*, void*);
struct STBOOT_SERIAL_IO_PROTOCOL {
    STBOOT_SERIAL_FN_RESET Reset;
    STBOOT_SERIAL_FN_SET_ATTRIBUTES SetAttributes;
    STBOOT_SERIAL_FN_SET_CONTROL SetControl;
    STBOOT_SERIAL_FN_GET_CONTROL GetControl;
    STBOOT_SERIAL_FN_TRANSFER Write;
    STBOOT_SERIAL_FN_TRANSFER Read;
    void* Mode;
    uint32_t Revision;
};

static STBOOT_SERIAL_IO_PROTOCOL* gStbootSerial = nullptr;
static bool gStbootSerialDiscoveryDone = false;

static STBOOT_SERIAL_IO_PROTOCOL* FindStbootSerial(EFI_BOOT_SERVICES* BS) {
    if (gStbootSerial || gStbootSerialDiscoveryDone || !BS) return gStbootSerial;
    gStbootSerialDiscoveryDone = true;
    EFI_GUID serialIoGuid = {0xBB25CF6F, 0xF1D4, 0x11D2,
                             {0x9A, 0x0C, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0xFD}};
    UINTN handleCount = 0;
    EFI_HANDLE* handles = nullptr;
    // UEFI LocateHandleSearchType.ByProtocol == 2.
    if (BS->LocateHandleBuffer(2, &serialIoGuid, nullptr,
                               &handleCount, &handles) != EFI_SUCCESS) return nullptr;
    for (UINTN i = 0; i < handleCount; ++i) {
        STBOOT_SERIAL_IO_PROTOCOL* serial = nullptr;
        if (BS->HandleProtocol(handles[i], &serialIoGuid,
                               reinterpret_cast<void**>(&serial)) == EFI_SUCCESS &&
            serial && serial->Write) {
            gStbootSerial = serial;
            break;
        }
    }
    if (handles) BS->FreePool(handles);
    return gStbootSerial;
}

static void StbootSerialLog(EFI_BOOT_SERVICES* BS, const char* text) {
    STBOOT_SERIAL_IO_PROTOCOL* serial = FindStbootSerial(BS);
    if (!serial || !text) return;
    UINTN bytes = 0;
    while (text[bytes] != '\0') ++bytes;
    if (bytes == 0) return;
    (void)serial->Write(serial, &bytes, const_cast<char*>(text));
}

'''

marker = "// Returns only if RXFS/APFS could not prepare and load boot.efi. Once the\n"
if src.count(marker) != 1:
    raise SystemExit("ABORT: expected TryRxfsBootEfi comment marker not found exactly once")
src = src.replace(marker, helper + marker, 1)

# Each anchor is checked in memory before the source is backed up/written.
def replace_once(old: str, new: str, desc: str) -> None:
    global src
    count = src.count(old)
    if count != 1:
        raise SystemExit(f"ABORT: {desc} anchor found {count} times (expected 1); no source written")
    src = src.replace(old, new, 1)

replace_once(
    "    EFI_STATUS status = RxfsMount(BS, &rxfs);\n",
    '    StbootSerialLog(BS, "[STBOOT] RXFS mount attempt\\r\\n");\n'
    "    EFI_STATUS status = RxfsMount(BS, &rxfs);\n"
    '    StbootSerialLog(BS, status == EFI_SUCCESS ? "[STBOOT] RXFS mount PASS\\r\\n" : "[STBOOT] RXFS mount FAIL\\r\\n");\n',
    "RXFS mount",
)
replace_once(
    "    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n",
    '    StbootSerialLog(BS, "[STBOOT] BaseSystem/APFS open attempt\\r\\n");\n'
    "    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n"
    '    StbootSerialLog(BS, status == EFI_SUCCESS ? "[STBOOT] BaseSystem/APFS open PASS\\r\\n" : "[STBOOT] BaseSystem/APFS open FAIL\\r\\n");\n',
    "BaseSystem open",
)
replace_once(
    "    status = StbootGetBootEfiInfo(&baseSystem, &bootInfo);\n",
    '    StbootSerialLog(BS, "[STBOOT] boot.efi metadata lookup attempt\\r\\n");\n'
    "    status = StbootGetBootEfiInfo(&baseSystem, &bootInfo);\n"
    '    StbootSerialLog(BS, status == EFI_SUCCESS ? "[STBOOT] boot.efi metadata PASS\\r\\n" : "[STBOOT] boot.efi metadata FAIL\\r\\n");\n',
    "boot.efi metadata lookup",
)
load_call = (
    "    status = BS->LoadImage(FALSE, ImageHandle, nullptr, imageBuffer,\n"
    "                           static_cast<UINTN>(bootInfo.Size), &childImage);\n"
)
load_replacement = (
    '    StbootSerialLog(BS, "[STBOOT] boot.efi bytes read PASS; UEFI LoadImage attempt\\r\\n");\n'
    + load_call
    + '    StbootSerialLog(BS, status == EFI_SUCCESS && childImage ? "[STBOOT] UEFI LoadImage PASS\\r\\n" : "[STBOOT] UEFI LoadImage FAIL\\r\\n");\n'
)
replace_once(load_call, load_replacement, "boot.efi LoadImage call")
replace_once(
    "    childLoadedImage->LoadOptions = &handoff;\n",
    "    childLoadedImage->LoadOptions = &handoff;\n"
    '    StbootSerialLog(BS, "[STBOOT] read-only filesystem handoff assigned\\r\\n");\n',
    "LoadOptions handoff",
)
# First occurrence is the RXFS boot.efi child path; legacy fallback uses the same StartImage expression later.
start_line = "    (void)BS->StartImage(childImage, nullptr, nullptr);\n"
count = src.count(start_line)
if count < 1:
    raise SystemExit("ABORT: StartImage call not found; no source written")
src = src.replace(
    start_line,
    '    StbootSerialLog(BS, "[STBOOT] Starting Starlix boot.efi via StartImage\\r\\n");\n' + start_line
    + '    StbootSerialLog(BS, "[STBOOT] boot.efi returned; halting instead of SSP fallback\\r\\n");\n',
    1,
)
replace_once(
    "static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n",
    "static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n"
    '    StbootSerialLog(BS, "[STBOOT] Legacy SSP fallback begin\\r\\n");\n',
    "legacy fallback entry",
)
replace_once(
    "    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);\n",
    '    StbootSerialLog(BS, "[STBOOT] stbootmngr entry; RXFS-first path begin\\r\\n");\n'
    "    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);\n",
    "RXFS-first entry",
)
replace_once(
    "    (void)rxfsStatus;\n    RunLegacySspFallback(ImageHandle, BS);\n",
    "    (void)rxfsStatus;\n"
    '    StbootSerialLog(BS, "[STBOOT] RXFS-first path returned; attempting SSP fallback\\r\\n");\n'
    "    RunLegacySspFallback(ImageHandle, BS);\n",
    "SSP fallback call",
)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = MAIN.with_name(MAIN.name + ".bak-serial-phase10-v2-" + stamp)
backup.write_bytes(MAIN.read_bytes())
MAIN.write_text(src, encoding="utf-8", newline="\n")
print("Backup :", backup.relative_to(ROOT))
print("Updated:", MAIN.relative_to(ROOT))
print("Added serial diagnostics around RXFS, BaseSystem/APFS, metadata, LoadImage, handoff and fallback.")
print("This adds no block writes and no ExitBootServices call.")
