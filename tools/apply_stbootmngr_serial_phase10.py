#!/usr/bin/env python3
"""Add EFI Serial I/O diagnostics to stbootmngr. No disk writes are added."""
from pathlib import Path
from datetime import datetime
import re
import sys

root = Path(__file__).resolve().parents[1]
main = root / "boot/x64/stbootmngr.efi/src/main.cpp"
if not main.is_file():
    raise SystemExit(f"ABORT: source file missing: {main}")
src = main.read_text(encoding="utf-8")

helper_marker = "namespace {\nconstexpr uint64_t kMaxBootEfiBytes"
helper = r'''namespace {
// Optional UEFI Serial I/O diagnostics. The protocol is discovered dynamically;
// if firmware does not expose it, diagnostics are silently unavailable and boot
// behavior is unchanged. This helper performs no disk I/O or writes.
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
    // Serial I/O Write takes a mutable buffer pointer by UEFI ABI, but does not
    // modify the caller's diagnostic string.
    (void)serial->Write(serial, &bytes, const_cast<char*>(text));
}

constexpr uint64_t kMaxBootEfiBytes'''

if src.count(helper_marker) != 1:
    raise SystemExit("ABORT: expected namespace/constant marker not found exactly once")
src = src.replace(helper_marker, helper, 1)

repls = [
    ('    EFI_STATUS status = RxfsMount(BS, &rxfs);\n    if (status != EFI_SUCCESS) return status;\n',
     '    StbootSerialLog(BS, "[STBOOT] RXFS mount attempt\\r\\n");\n    EFI_STATUS status = RxfsMount(BS, &rxfs);\n    if (status != EFI_SUCCESS) {\n        StbootSerialLog(BS, "[STBOOT] RXFS mount FAIL; returning to legacy path\\r\\n");\n        return status;\n    }\n    StbootSerialLog(BS, "[STBOOT] RXFS mount PASS\\r\\n");\n'),
    ('    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n    if (status != EFI_SUCCESS) return status;\n',
     '    StbootSerialLog(BS, "[STBOOT] BaseSystem/APFS open attempt\\r\\n");\n    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);\n    if (status != EFI_SUCCESS) {\n        StbootSerialLog(BS, "[STBOOT] BaseSystem/APFS open FAIL; returning to legacy path\\r\\n");\n        return status;\n    }\n    StbootSerialLog(BS, "[STBOOT] BaseSystem/APFS open PASS\\r\\n");\n'),
    ('    (void)BS->StartImage(childImage, nullptr, nullptr);\n    // Once boot.efi has been entered, never jump back to SSP: it may have\n',
     '    StbootSerialLog(BS, "[STBOOT] Starting Starlix boot.efi via StartImage\\r\\n");\n    (void)BS->StartImage(childImage, nullptr, nullptr);\n    StbootSerialLog(BS, "[STBOOT] boot.efi returned; halt (no legacy fallback after entry)\\r\\n");\n    // Once boot.efi has been entered, never jump back to SSP: it may have\n'),
    ('static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n    SSP_CONTEXT ssp;\n',
     'static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {\n    StbootSerialLog(BS, "[STBOOT] Legacy SSP fallback begin\\r\\n");\n    SSP_CONTEXT ssp;\n'),
    ('    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);\n    (void)rxfsStatus;\n    RunLegacySspFallback(ImageHandle, BS);\n',
     '    StbootSerialLog(BS, "[STBOOT] stbootmngr entry; RXFS-first path begin\\r\\n");\n    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);\n    (void)rxfsStatus;\n    StbootSerialLog(BS, "[STBOOT] RXFS-first path returned; attempting SSP fallback\\r\\n");\n    RunLegacySspFallback(ImageHandle, BS);\n'),
]
load_pattern = re.compile(
    r'    status = BS->LoadImage\(FALSE, ImageHandle, nullptr, imageBuffer,\n\s+static_cast<UINTN>\(bootInfo\.Size\), &childImage\);\n'
)
load_replacement = (
    '    StbootSerialLog(BS, "[STBOOT] boot.efi bytes read PASS; UEFI LoadImage attempt\\r\\n");\n'
    '    status = BS->LoadImage(FALSE, ImageHandle, nullptr, imageBuffer,\n'
    '                           static_cast<UINTN>(bootInfo.Size), &childImage);\n'
)
src, load_count = load_pattern.subn(lambda _match: load_replacement, src, count=1)
if load_count != 1:
    raise SystemExit("ABORT: boot.efi LoadImage anchor not found exactly once")

child_failure_pattern = re.compile(
    r'    if \(status != EFI_SUCCESS \|\| !childImage\)\n\s+return status == EFI_SUCCESS \? EFI_LOAD_ERROR : status;\n'
)
child_failure_replacement = (
    '    if (status != EFI_SUCCESS || !childImage) {\n'
    '        StbootSerialLog(BS, "[STBOOT] UEFI LoadImage FAIL; returning to legacy path\\r\\n");\n'
    '        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;\n'
    '    }\n'
    '    StbootSerialLog(BS, "[STBOOT] UEFI LoadImage PASS; setting filesystem handoff\\r\\n");\n'
)
src, child_failure_count = child_failure_pattern.subn(lambda _match: child_failure_replacement, src, count=1)
if child_failure_count != 1:
    raise SystemExit("ABORT: boot.efi LoadImage result anchor not found exactly once")

for old, new in repls:
    if src.count(old) != 1:
        raise SystemExit("ABORT: expected insertion marker not found exactly once:\n" + old[:160])
    src = src.replace(old, new, 1)

stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
backup = main.with_name(main.name + ".bak-serial-phase10-" + stamp)
backup.write_bytes(main.read_bytes())
main.write_text(src, encoding="utf-8")
print("Backup :", backup.relative_to(root))
print("Updated:", main.relative_to(root))
print("Added optional EFI Serial I/O diagnostics for RXFS/BaseSystem/LoadImage/fallback.")
print("No disk writes or ExitBootServices changes were added.")
