// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
// ============================================================
// Starlix OS - SSPFS (Starlix System Partition File System)
// ------------------------------------------------------------
// A proprietary, intentionally non-standard, READ-ONLY raw disk
// format. There is no driver for this format on Windows, Linux,
// macOS, or any other operating system. Only the Starlix Boot
// Manager (stbtmngr.efi) contains the logic to parse it, via
// direct EFI_DISK_IO_PROTOCOL byte-level reads - bypassing any
// standard EFI_SIMPLE_FILE_SYSTEM_PROTOCOL entirely.
//
// NOTE: This header assumes fixed-width integer type aliases
// (uint8_t, uint32_t, uint64_t) are ALREADY DEFINED by whatever
// includes this file first:
//   - On the EFI (freestanding) side: include "efi/efi.hpp" first.
//   - On the host tool (hosted) side: include <cstdint> first.
// ============================================================

#pragma pack(push, 1)

#define SSP_MAGIC        "STARLIX1"   // 8-byte magic signature
#define SSP_VERSION      1
#define SSP_FLAG_READONLY 0x00000001u

struct SSP_SUPERBLOCK {
    uint8_t  magic[8];              // "STARLIX1"
    uint32_t version;                // format version
    uint32_t flags;                  // bit0 = read-only (always set)
    uint64_t total_size;             // total partition size, bytes
    uint32_t file_count;             // number of entries in file table
    uint32_t file_table_offset;      // byte offset of file table
    uint32_t file_table_entry_size;  // sizeof(SSP_FILE_ENTRY)
    uint32_t data_start_offset;      // byte offset where file data begins
    uint8_t  volume_name[32];        // "STARLIX_SSP"
    uint32_t checksum;               // reserved, simple integrity check
    uint8_t  reserved[8];            // future use / padding
};

struct SSP_FILE_ENTRY {
    uint8_t  name[100];   // full path, e.g. "/BOOT/KERNEL.ELF"
    uint64_t offset;      // absolute byte offset within the partition
    uint64_t size;        // file size in bytes
    uint32_t attributes;  // reserved for future flags
    uint8_t  reserved[12];
};

#pragma pack(pop)
