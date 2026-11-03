#!/usr/bin/env python3
"""Add an opt-in APFS handoff kernel-package reader to Rxload.efi.

The legacy SSP/ECT path remains unchanged when no valid STBOOT_FS_HANDOFF is
supplied. The filesystem path is kernel-only: module loading is deliberately
left disabled until the APFS image gains an explicit module manifest/API.
"""
from pathlib import Path
from datetime import datetime
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "boot/x64/rxload.efi/src/main.cpp"
if not MAIN.is_file():
    raise SystemExit(f"ABORT: missing Rxload source: {MAIN}")
source = MAIN.read_text()

include_anchor = '#include "boot/bootinfo.hpp"'
if source.count(include_anchor) != 1:
    raise SystemExit("ABORT: Rxload include anchor differs; no files changed")
if '#include "stboot_handoff.hpp"' in source:
    raise SystemExit("ABORT: Phase 12 handoff include already exists; inspect source manually")

efi_main_anchor = 'extern "C" EFI_STATUS ABI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {'
if source.count(efi_main_anchor) != 1:
    raise SystemExit("ABORT: efi_main anchor differs; no files changed")

old_boot_loading_start = 'PrintAt(out, 2, 12, u"[*] Mounting SSP...");'
old_boot_loading_end = 'ECT_HDR* hdr = static_cast<ECT_HDR*>(ectBuf);'
if source.count(old_boot_loading_start) != 1 or source.count(old_boot_loading_end) != 1:
    raise SystemExit("ABORT: SSP-to-ECT source block differs; no files changed")
start = source.index(old_boot_loading_start)
end = source.index(old_boot_loading_end, start) + len(old_boot_loading_end)
if end <= start:
    raise SystemExit("ABORT: invalid source block ordering; no files changed")

module_start = 'if (!PrepareSystemModuleHandoff('
module_end = 'PrintAt(out, 2, 19, u"[+] External module inventory prepared");'
if source.count(module_start) != 1 or source.count(module_end) != 1:
    raise SystemExit("ABORT: module handoff source block differs; no files changed")
mod_s = source.index(module_start)
mod_e = source.index(module_end, mod_s) + len(module_end)
if mod_e <= mod_s:
    raise SystemExit("ABORT: invalid module block ordering; no files changed")

map_start_anchor = 'UINTN mapSize = 0, mapKey = 0, descSize = 0;'
map_end_anchor = '// No more UEFI calls. Jump to kernel with SysV ABI.'
if source.count(map_start_anchor) != 1 or source.count(map_end_anchor) != 1:
    raise SystemExit("ABORT: memory-map source block differs; no files changed")
map_s = source.index(map_start_anchor)
map_e = source.index(map_end_anchor, map_s)
if map_e <= map_s:
    raise SystemExit("ABORT: invalid memory-map block ordering; no files changed")

helper = r'''
// Validate the versioned filesystem contract passed in EFI_LOADED_IMAGE_PROTOCOL.LoadOptions.
// If no LoadOptions are present, the legacy standalone SSP boot path remains available.
static EFI_STATUS GetStbootFsHandoff(
    EFI_BOOT_SERVICES* BS, EFI_HANDLE ImageHandle,
    bool* supplied, STBOOT_FS_HANDOFF** output) {
    if (!BS || !supplied || !output) return EFI_INVALID_PARAMETER;
    *supplied = false;
    *output = nullptr;

    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    if (BS->HandleProtocol(ImageHandle, &loadedImageGuid,
                           reinterpret_cast<void**>(&loaded)) != EFI_SUCCESS ||
        !loaded) {
        // An image not started by stbootmngr may continue using legacy SSP boot.
        return EFI_SUCCESS;
    }

    if (!loaded->LoadOptions && loaded->LoadOptionsSize == 0)
        return EFI_SUCCESS;

    *supplied = true;
    if (!loaded->LoadOptions ||
        loaded->LoadOptionsSize != sizeof(STBOOT_FS_HANDOFF))
        return EFI_INVALID_PARAMETER;

    auto* handoff = static_cast<STBOOT_FS_HANDOFF*>(loaded->LoadOptions);
    const uint8_t expected[8] = {'S','T','B','F','S','0','0','1'};
    for (UINTN i = 0; i < 8; ++i) {
        if (handoff->Signature[i] != expected[i]) return EFI_INVALID_PARAMETER;
    }
    const uint64_t requiredFlags =
        STBOOT_FS_HANDOFF_READ_ONLY |
        STBOOT_FS_HANDOFF_BOOT_SERVICES_REQUIRED |
        STBOOT_FS_HANDOFF_UTF8_EXACT_NAMES;
    if (handoff->Version != STBOOT_FS_HANDOFF_VERSION ||
        handoff->Size != sizeof(STBOOT_FS_HANDOFF) ||
        (handoff->Flags & requiredFlags) != requiredFlags ||
        !handoff->Context || !handoff->Lookup ||
        !handoff->GetInfo || !handoff->Read)
        return EFI_INVALID_PARAMETER;

    *output = handoff;
    return EFI_SUCCESS;
}

// Read the kernel's ECT package through the parent-owned APFS/RXFS handoff.
// Reads are chunked so a large ECT file never exceeds the APFS reader's per-call bound.
static EFI_STATUS ReadKernelEctFromHandoff(
    EFI_BOOT_SERVICES* BS, STBOOT_FS_HANDOFF* handoff,
    void** bufferOut, uint64_t* sizeOut) {
    if (bufferOut) *bufferOut = nullptr;
    if (sizeOut) *sizeOut = 0;
    if (!BS || !handoff || !bufferOut || !sizeOut)
        return EFI_INVALID_PARAMETER;

    // Preserve the existing SSP logical path. The APFS volume must contain this file.
    const char* kernelPath = "/Starlix/System/RxKernel1.0.ect";
    uint64_t fileId = 0;
    uint16_t directoryEntryFlags = 0;
    BOOLEAN isDirectory = FALSE;
    EFI_STATUS status = handoff->Lookup(
        handoff->Context, kernelPath, &fileId,
        &directoryEntryFlags, &isDirectory);
    (void)directoryEntryFlags;
    if (status != EFI_SUCCESS) return status;
    if (fileId == 0 || isDirectory) return EFI_INVALID_PARAMETER;

    STBOOT_FS_FILE_INFO info{};
    status = handoff->GetInfo(handoff->Context, fileId, &info);
    if (status != EFI_SUCCESS) return status;
    if (info.FileId != fileId || info.Size < sizeof(ECT_HDR) ||
        info.Size > 64ULL * 1024ULL * 1024ULL ||
        info.Size > static_cast<uint64_t>(~static_cast<UINTN>(0)))
        return EFI_INVALID_PARAMETER;

    void* buffer = nullptr;
    const UINTN totalSize = static_cast<UINTN>(info.Size);
    status = BS->AllocatePool(EfiLoaderData, totalSize, &buffer);
    if (status != EFI_SUCCESS || !buffer) return EFI_INVALID_PARAMETER;

    const UINTN chunkLimit = 256U * 1024U;
    uint64_t offset = 0;
    while (offset < info.Size) {
        const uint64_t remaining = info.Size - offset;
        const UINTN request = remaining > chunkLimit
            ? chunkLimit : static_cast<UINTN>(remaining);
        UINTN bytesRead = 0;
        status = handoff->Read(
            handoff->Context, fileId, offset, request,
            static_cast<uint8_t*>(buffer) + static_cast<UINTN>(offset),
            &bytesRead);
        if (status != EFI_SUCCESS || bytesRead != request) {
            (void)BS->FreePool(buffer);
            return status == EFI_SUCCESS ? EFI_INVALID_PARAMETER : status;
        }
        offset += bytesRead;
    }

    *bufferOut = buffer;
    *sizeOut = info.Size;
    return EFI_SUCCESS;
}

// Capture the final UEFI memory map and retry ExitBootServices only by refreshing
// the map. After the first ExitBootServices attempt, do not allocate, free, log,
// or call any other boot service unless the pre-attempt buffer proved too small.
static bool FinalizeMemoryMapAndExitBootServices(
    EFI_BOOT_SERVICES* BS, EFI_HANDLE imageHandle, StarlixBootInfo* bootInfo) {
    if (!BS || !bootInfo) return false;

    UINTN requiredSize = 0;
    UINTN mapKey = 0;
    UINTN descriptorSize = 0;
    UINT32 descriptorVersion = 0;
    EFI_STATUS status = BS->GetMemoryMap(
        &requiredSize, nullptr, &mapKey, &descriptorSize, &descriptorVersion);
    if (status != EFI_BUFFER_TOO_SMALL || requiredSize == 0 || descriptorSize == 0)
        return false;

    const UINTN maxValue = static_cast<UINTN>(~static_cast<UINTN>(0));
    if (descriptorSize > (maxValue - 16384U) / 128U) return false;
    UINTN capacitySlack = descriptorSize * 128U + 16384U;
    if (requiredSize > maxValue - capacitySlack) return false;
    UINTN capacity = requiredSize + capacitySlack;

    EFI_MEMORY_DESCRIPTOR* memoryMap = nullptr;
    status = BS->AllocatePool(
        EfiLoaderData, capacity, reinterpret_cast<void**>(&memoryMap));
    if (status != EFI_SUCCESS || !memoryMap) return false;

    bool attemptedExit = false;
    for (UINTN attempt = 0; attempt < 8; ++attempt) {
        UINTN mapSize = capacity;
        status = BS->GetMemoryMap(
            &mapSize, memoryMap, &mapKey, &descriptorSize, &descriptorVersion);
        if (status != EFI_SUCCESS) {
            if (status == EFI_BUFFER_TOO_SMALL && !attemptedExit &&
                descriptorSize != 0 &&
                descriptorSize <= (maxValue - 16384U) / 128U) {
                capacitySlack = descriptorSize * 128U + 16384U;
                if (mapSize > maxValue - capacitySlack) return false;
                const UINTN newCapacity = mapSize + capacitySlack;
                EFI_MEMORY_DESCRIPTOR* largerMap = nullptr;
                status = BS->AllocatePool(
                    EfiLoaderData, newCapacity,
                    reinterpret_cast<void**>(&largerMap));
                if (status != EFI_SUCCESS || !largerMap) return false;
                (void)BS->FreePool(memoryMap);
                memoryMap = largerMap;
                capacity = newCapacity;
                continue;
            }
            if (attemptedExit) {
                for (;;) asm volatile("hlt");
            }
            return false;
        }

        bootInfo->MemoryMapBase =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(memoryMap));
        bootInfo->MemoryMapSize = mapSize;
        bootInfo->MemoryMapDescriptorSize = descriptorSize;
        bootInfo->MemoryMapDescriptorVersion = descriptorVersion;

        status = BS->ExitBootServices(imageHandle, mapKey);
        if (status == EFI_SUCCESS) return true;
        attemptedExit = true;
        if (status != EFI_INVALID_PARAMETER) {
            for (;;) asm volatile("hlt");
        }
        // A stale key is retried from a fresh GetMemoryMap on the next iteration.
    }

    if (attemptedExit) {
        for (;;) asm volatile("hlt");
    }
    return false;
}

'''

new_loading_block = r'''      SSP_CONTEXT ssp{};
      static uint8_t fileTableBuf[64 * 1024];
      const SSP_FILE_ENTRY* kEntry = nullptr;
      void* ectBuf = nullptr;
      uint64_t ectSize = 0;

      bool fsHandoffSupplied = false;
      STBOOT_FS_HANDOFF* fsHandoff = nullptr;
      EFI_STATUS handoffStatus = GetStbootFsHandoff(
          BS, ImageHandle, &fsHandoffSupplied, &fsHandoff);
      if (fsHandoffSupplied && handoffStatus != EFI_SUCCESS) {
          PrintAt(out, 2, 13, u"[-] Invalid filesystem handoff");
          for (;;) asm volatile ("hlt");
      }

      if (fsHandoff) {
          PrintAt(out, 2, 12, u"[*] Reading kernel package from APFS...");
          handoffStatus = ReadKernelEctFromHandoff(
              BS, fsHandoff, &ectBuf, &ectSize);
          if (handoffStatus != EFI_SUCCESS || !ectBuf) {
              PrintAt(out, 2, 13, u"[-] APFS kernel package read failed");
              for (;;) asm volatile ("hlt");
          }
          PrintAt(out, 2, 14, u"[+] APFS kernel package read");
      } else {
          PrintAt(out, 2, 12, u"[*] Mounting SSP...");
          if (SspMount(BS, &ssp) != EFI_SUCCESS) {
              PrintAt(out, 2, 13, u"[-] SSP mount failed");
              for (;;) asm volatile ("hlt");
          }
          PrintAt(out, 2, 13, u"[+] SSP mounted");

          UINTN ftSize = (UINTN)ssp.Superblock.file_count *
                         (UINTN)ssp.Superblock.file_table_entry_size;
          if (ftSize > sizeof(fileTableBuf) ||
              SspReadFileTable(&ssp, fileTableBuf, sizeof(fileTableBuf)) != EFI_SUCCESS) {
              PrintAt(out, 2, 14, u"[-] File table read failed");
              for (;;) asm volatile ("hlt");
          }

          kEntry = SspFindFile(
              fileTableBuf, ssp.Superblock.file_count,
              "/Starlix/System/RxKernel1.0.ect");
          if (!kEntry) {
              PrintAt(out, 2, 14, u"[-] /Starlix/System/RxKernel1.0.ect not found");
              for (;;) asm volatile ("hlt");
          }
          PrintAt(out, 2, 14, u"[+] Found RxKernel1.0.ect");

          if (kEntry->size < sizeof(ECT_HDR) ||
              kEntry->size > 64ULL * 1024ULL * 1024ULL) {
              PrintAt(out, 2, 15, u"[-] Invalid kernel package size");
              for (;;) asm volatile ("hlt");
          }
          ectSize = kEntry->size;
          if (BS->AllocatePool(EfiLoaderData, kEntry->size, &ectBuf) != EFI_SUCCESS ||
              !ectBuf) {
              PrintAt(out, 2, 15, u"[-] AllocatePool failed");
              for (;;) asm volatile ("hlt");
          }
          if (SspReadFile(&ssp, kEntry, ectBuf) != EFI_SUCCESS) {
              PrintAt(out, 2, 15, u"[-] Kernel read failed");
              for (;;) asm volatile ("hlt");
          }
      }

      for (UINTN percent = 60; percent <= 85; percent += 5) {
          DrawProgressBar(out, percent);
          BS->Stall(15000);
      }

      ECT_HDR* hdr = static_cast<ECT_HDR*>(ectBuf);'''

new_module_block = r'''      if (fsHandoff) {
          // This phase deliberately boots the kernel without external modules:
          // the handoff ABI has file lookup/read, but no directory-listing API.
          // Module manifest discovery and custom .kext loading are a later phase.
          bootInfo.SystemModuleTableBase = 0;
          bootInfo.SystemModuleCount = 0;
          bootInfo.SystemModuleDescriptorSize =
              static_cast<unsigned int>(sizeof(StarlixBootModule));
          PrintAt(out, 2, 19, u"[!] APFS kernel-only stage; modules deferred");
      } else {
          if (!PrepareSystemModuleHandoff(
                  BS, &ssp, fileTableBuf, sizeof(fileTableBuf), &bootInfo)) {
              PrintAt(out, 2, 19, u"[-] External module handoff failed");
              for (;;) asm volatile ("hlt");
          }
          PrintAt(out, 2, 19, u"[+] External module inventory prepared");
      }'''

new_map_block = r'''      if (!FinalizeMemoryMapAndExitBootServices(BS, ImageHandle, &bootInfo)) {
          PrintAt(out, 2, 18, u"[-] Memory map preparation failed");
          for (;;) asm volatile ("hlt");
      }

'''

# Prepare all changes in memory, after all anchors have passed.
source = source.replace(include_anchor, include_anchor + '\n#include "stboot_handoff.hpp"', 1)
# Helper must be placed after ECT_HDR and related helper declarations, before efi_main.
source = source.replace(efi_main_anchor, helper + efi_main_anchor, 1)
# Recalculate offsets after insertion above changed the source length.
start = source.index(old_boot_loading_start)
end = source.index(old_boot_loading_end, start) + len(old_boot_loading_end)
source = source[:start] + new_loading_block + source[end:]
# Recalculate later anchors after first replacement.
if source.count(module_start) != 1 or source.count(module_end) != 1:
    raise SystemExit("ABORT: module anchors changed during preparation; no files written")
mod_s = source.index(module_start)
mod_e = source.index(module_end, mod_s) + len(module_end)
source = source[:mod_s] + new_module_block + source[mod_e:]
if source.count(map_start_anchor) != 1:
    raise SystemExit("ABORT: memory-map anchor changed during preparation; no files written")
map_s = source.index(map_start_anchor)
map_e = source.index(map_end_anchor, map_s)
source = source[:map_s] + new_map_block + source[map_e:]

backup = MAIN.with_name(
    MAIN.name + ".bak-phase12-apfs-kernel-source-" +
    datetime.now().strftime("%Y%m%d_%H%M%S")
)
shutil.copy2(MAIN, backup)
MAIN.write_text(source)
print("Updated:", MAIN)
print("Backup :", backup)
print("Phase 12 adds opt-in APFS ECT input, preserves standalone SSP mode, defers APFS modules, and hardens map retry logic.")
