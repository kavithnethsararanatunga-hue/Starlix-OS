#include "efi/efi.hpp"
#include "boot/x64/stbootmngr.efi/src/stboot_handoff.hpp"

namespace {
static CHAR16 kPass[] = u"\r\n[PHASE11 CHILD] filesystem handoff PASS; valid PE read; returning EFI_SUCCESS\r\n";
static CHAR16 kFail[] = u"\r\n[PHASE11 CHILD] filesystem handoff FAIL\r\n";

static EFI_STATUS Fail(EFI_SYSTEM_TABLE* st) {
    if (st && st->ConOut && st->ConOut->OutputString)
        (void)st->ConOut->OutputString(st->ConOut, kFail);
    return EFI_LOAD_ERROR;
}

static bool SigEquals(const uint8_t* p, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (p[i] != static_cast<uint8_t>(s[i])) return false;
    return true;
}
static uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

extern "C" EFI_STATUS ABI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    if (!SystemTable || !SystemTable->BootServices || !SystemTable->BootServices->HandleProtocol)
        return Fail(SystemTable);

    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    EFI_STATUS status = SystemTable->BootServices->HandleProtocol(
        ImageHandle, &loadedImageGuid, reinterpret_cast<void**>(&loaded));
    if (status != EFI_SUCCESS || !loaded || !loaded->LoadOptions ||
        loaded->LoadOptionsSize != sizeof(STBOOT_FS_HANDOFF)) return Fail(SystemTable);

    const auto* handoff = static_cast<const STBOOT_FS_HANDOFF*>(loaded->LoadOptions);
    static const uint8_t expectedSignature[8] = {'S','T','B','F','S','0','0','1'};
    for (uint32_t i = 0; i < 8; ++i)
        if (handoff->Signature[i] != expectedSignature[i]) return Fail(SystemTable);

    if (handoff->Version != STBOOT_FS_HANDOFF_VERSION ||
        handoff->Size != sizeof(STBOOT_FS_HANDOFF) || !handoff->Context ||
        !handoff->Lookup || !handoff->GetInfo || !handoff->Read ||
        !(handoff->Flags & STBOOT_FS_HANDOFF_READ_ONLY) ||
        !(handoff->Flags & STBOOT_FS_HANDOFF_BOOT_SERVICES_REQUIRED)) return Fail(SystemTable);

    uint64_t fileId = 0;
    uint16_t dirFlags = 0;
    BOOLEAN isDirectory = TRUE;
    status = handoff->Lookup(handoff->Context,
        "/System/Library/CoreServices/boot.efi", &fileId, &dirFlags, &isDirectory);
    if (status != EFI_SUCCESS || fileId == 0 || isDirectory != FALSE) return Fail(SystemTable);

    STBOOT_FS_FILE_INFO info{};
    status = handoff->GetInfo(handoff->Context, fileId, &info);
    if (status != EFI_SUCCESS || info.FileId != fileId || info.Size < 64 ||
        info.Size > 64ULL * 1024ULL * 1024ULL || (info.Mode & 0xF000U) != 0x8000U)
        return Fail(SystemTable);

    uint8_t header[64]{};
    UINTN got = 0;
    status = handoff->Read(handoff->Context, fileId, 0, sizeof(header), header, &got);
    if (status != EFI_SUCCESS || got != sizeof(header) || header[0] != 'M' || header[1] != 'Z')
        return Fail(SystemTable);
    const uint32_t peOffset = Le32(header + 0x3c);
    if (peOffset < 64 || static_cast<uint64_t>(peOffset) + 4 > info.Size) return Fail(SystemTable);

    uint8_t peSignature[4]{};
    got = 0;
    status = handoff->Read(handoff->Context, fileId, peOffset, sizeof(peSignature), peSignature, &got);
    if (status != EFI_SUCCESS || got != sizeof(peSignature) || !SigEquals(peSignature, "PE\0\0", 4))
        return Fail(SystemTable);

    // Fixture file size is exactly 4100 bytes. This validates cross-extent
    // reading and EOF clamping through the filesystem handoff callback.
    uint8_t tail[8]{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    got = 0;
    status = handoff->Read(handoff->Context, fileId, 4096, sizeof(tail), tail, &got);
    if (status != EFI_SUCCESS || got != 4 || tail[0] || tail[1] || tail[2] || tail[3])
        return Fail(SystemTable);

    if (SystemTable->ConOut && SystemTable->ConOut->OutputString)
        (void)SystemTable->ConOut->OutputString(SystemTable->ConOut, kPass);
    return EFI_SUCCESS;
}
