// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "efi/efi.hpp"
#include "efi/runtime.hpp"
#include "ssp.hpp"
#include "rxfs_reader.hpp"
#include "base_system_reader.hpp"
#include "stboot_handoff.hpp"

namespace {
constexpr uint64_t kMaxBootEfiBytes = 64ULL * 1024ULL * 1024ULL;
constexpr UINTN kBootEfiReadChunk = 64ULL * 1024ULL;

typedef EFI_STATUS (ABI *EFI_UNLOAD_IMAGE_FN)(EFI_HANDLE ImageHandle);

[[noreturn]] static void HaltForever() {
    for (;;) asm volatile ("hlt");
}

// Returns only if RXFS/APFS could not prepare and load boot.efi. Once the
// child starts, this function never returns, even if StartImage does.
static EFI_STATUS TryRxfsBootEfi(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {
    static RXFS_CONTEXT rxfs{};
    static UDIF_CONTEXT udif{}; // large run table must remain outside the stack
    static STBOOT_BASESYSTEM_CONTEXT baseSystem{};
    static STBOOT_FS_HANDOFF handoff{};

    EFI_STATUS status = RxfsMount(BS, &rxfs);
    if (status != EFI_SUCCESS) return status;

    status = StbootOpenBaseSystem(&rxfs, 0, &udif, &baseSystem);
    if (status != EFI_SUCCESS) return status;

    APFS_FILE_INFO bootInfo{};
    status = StbootGetBootEfiInfo(&baseSystem, &bootInfo);
    if (status != EFI_SUCCESS) return status;
    if (bootInfo.Size == 0 || bootInfo.Size > kMaxBootEfiBytes ||
        bootInfo.Size > static_cast<uint64_t>(~static_cast<UINTN>(0)))
        return EFI_LOAD_ERROR;

    status = StbootCreateFsHandoff(&baseSystem, &handoff);
    if (status != EFI_SUCCESS) return status;

    void* imageBuffer = nullptr;
    status = BS->AllocatePool(EfiLoaderData, static_cast<UINTN>(bootInfo.Size), &imageBuffer);
    if (status != EFI_SUCCESS || !imageBuffer)
        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;

    uint64_t offset = 0;
    while (offset < bootInfo.Size) {
        const uint64_t remain = bootInfo.Size - offset;
        const UINTN want = static_cast<UINTN>(remain > kBootEfiReadChunk
                                                  ? kBootEfiReadChunk : remain);
        UINTN got = 0;
        status = StbootReadBootEfi(&baseSystem, offset, want,
                                   static_cast<uint8_t*>(imageBuffer) + offset, &got);
        if (status != EFI_SUCCESS || got != want) {
            BS->FreePool(imageBuffer);
            return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;
        }
        offset += got;
    }

    EFI_HANDLE childImage = nullptr;
    status = BS->LoadImage(FALSE, ImageHandle, nullptr, imageBuffer,
                           static_cast<UINTN>(bootInfo.Size), &childImage);
    BS->FreePool(imageBuffer);
    imageBuffer = nullptr;
    if (status != EFI_SUCCESS || !childImage)
        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;

    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* childLoadedImage = nullptr;
    status = BS->HandleProtocol(childImage, &loadedImageGuid,
                                reinterpret_cast<void**>(&childLoadedImage));
    if (status != EFI_SUCCESS || !childLoadedImage) {
        auto unload = reinterpret_cast<EFI_UNLOAD_IMAGE_FN>(BS->UnloadImage);
        if (unload) unload(childImage);
        return status == EFI_SUCCESS ? EFI_LOAD_ERROR : status;
    }

    // Custom Starlix boot.efi contract. The structure and callback code remain
    // alive in the parent image while the child is running. The child must read
    // the kernel and extensions before ExitBootServices(); this is read-only.
    childLoadedImage->LoadOptionsSize = static_cast<uint32_t>(sizeof(handoff));
    childLoadedImage->LoadOptions = &handoff;

    (void)BS->StartImage(childImage, nullptr, nullptr);
    // Once boot.efi has been entered, never jump back to SSP: it may have
    // changed boot state or attempted ExitBootServices before returning.
    HaltForever();
}

// Transitional compatibility path. Keep this until RXFS/APFS is verified with
// a real BaseSystem image in an isolated VM. This branch is not the target
// architecture and must be removable after end-to-end runtime validation.
static void RunLegacySspFallback(EFI_HANDLE ImageHandle, EFI_BOOT_SERVICES* BS) {
    SSP_CONTEXT ssp;
    EFI_STATUS status = SspMount(BS, &ssp);
    if (status != EFI_SUCCESS) HaltForever();

    static uint8_t fileTableBuf[64 * 1024];
    UINTN ftSize = static_cast<UINTN>(ssp.Superblock.file_count) *
                   static_cast<UINTN>(ssp.Superblock.file_table_entry_size);
    if (ftSize > sizeof(fileTableBuf) ||
        SspReadFileTable(&ssp, fileTableBuf, sizeof(fileTableBuf)) != EFI_SUCCESS)
        HaltForever();

    const SSP_FILE_ENTRY* rxloadEntry =
        SspFindFile(fileTableBuf, ssp.Superblock.file_count,
                    "/Starlix/System/Boot/Rxload.efi");
    if (!rxloadEntry || rxloadEntry->size == 0) HaltForever();

    void* efiBuffer = nullptr;
    status = BS->AllocatePool(EfiLoaderData, rxloadEntry->size, &efiBuffer);
    if (status != EFI_SUCCESS || !efiBuffer) HaltForever();

    status = SspReadFile(&ssp, rxloadEntry, efiBuffer);
    if (status != EFI_SUCCESS) {
        BS->FreePool(efiBuffer);
        HaltForever();
    }

    EFI_HANDLE childImage = nullptr;
    status = BS->LoadImage(FALSE, ImageHandle, nullptr, efiBuffer,
                           rxloadEntry->size, &childImage);
    BS->FreePool(efiBuffer);
    if (status != EFI_SUCCESS || !childImage) HaltForever();

    (void)BS->StartImage(childImage, nullptr, nullptr);
    HaltForever();
}
} // namespace

extern "C" EFI_STATUS ABI efi_main(EFI_HANDLE ImageHandle,
                                    EFI_SYSTEM_TABLE* SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) HaltForever();
    EFI_BOOT_SERVICES* BS = SystemTable->BootServices;

    const EFI_STATUS rxfsStatus = TryRxfsBootEfi(ImageHandle, BS);
    (void)rxfsStatus;
    RunLegacySspFallback(ImageHandle, BS);
    HaltForever();
}
