// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
// ============================================================
// Starlix OS - EFI Block I/O & Disk I/O Protocol Definitions
// Required to read raw SSP partition sectors directly, without
// relying on any standard EFI_SIMPLE_FILE_SYSTEM_PROTOCOL driver.
// ============================================================
#include "efi/efi.hpp"

extern "C" {

// ------------------- EFI_BLOCK_IO_PROTOCOL -------------------
struct EFI_BLOCK_IO_MEDIA {
    uint32_t MediaId;
    BOOLEAN  RemovableMedia;
    BOOLEAN  MediaPresent;
    BOOLEAN  LogicalPartition;
    BOOLEAN  ReadOnly;
    BOOLEAN  WriteCaching;
    uint8_t  _pad[3];
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint64_t LastBlock;
};

struct EFI_BLOCK_IO_PROTOCOL {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA* Media;
    void* Reset;
    void* ReadBlocks;
    void* WriteBlocks;
    void* FlushBlocks;
};

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b} }

// ------------------- EFI_DISK_IO_PROTOCOL -------------------
struct EFI_DISK_IO_PROTOCOL;

typedef EFI_STATUS (ABI *EFI_DISK_READ)(EFI_DISK_IO_PROTOCOL*, UINT32, UINT64, UINTN, void*);
typedef EFI_STATUS (ABI *EFI_DISK_WRITE)(EFI_DISK_IO_PROTOCOL*, UINT32, UINT64, UINTN, void*);

struct EFI_DISK_IO_PROTOCOL {
    uint64_t Revision;
    EFI_DISK_READ  ReadDisk;
    EFI_DISK_WRITE WriteDisk;
};

#define EFI_DISK_IO_PROTOCOL_GUID \
    { 0xce345171, 0xba0b, 0x11d2, {0x8e,0x4f,0x00,0xa0,0xc9,0x69,0x72,0x3b} }

} // extern "C"
