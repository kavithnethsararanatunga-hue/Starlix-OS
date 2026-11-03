/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once

#include "../drivers/ehci.hpp"
#include "../rxfs_native.hpp"

namespace starlix::storage {

constexpr uint32_t kMaxLogicalBlockBytes = 4096;
constexpr size_t kMaxBlockDevices = 8;
constexpr size_t kMaxPartitionsPerDevice = 16;

enum class BlockBackend : uint8_t {
    None = 0,
    UsbMassStorage = 1,
    AtaPio = 2,
};

struct BlockDevice {
    bool present;
    BlockBackend backend;
    uint8_t ehci_address;
    uint8_t ata_channel;
    uint8_t ata_drive;
    uint16_t ata_command_base;
    uint16_t ata_control_port;
    bool ata_lba48;
    uint64_t last_lba;
    uint32_t block_size;
};

struct Partition {
    bool present;
    bool gpt;
    uint8_t index;
    uint8_t mbr_type;
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t type_guid[16];
};

struct RxfsVolume {
    bool mounted;
    BlockDevice device;
    Partition partition;
    uint64_t total_blocks;
    starlix::rxfs::RXFS_SUPERBLOCK superblock;
};

struct Fat32Volume {
    bool mounted;
    BlockDevice device;
    Partition partition;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t first_data_sector;
    uint32_t total_sectors;
    uint32_t cluster_count;
    uint32_t active_fat;
};

/* Bounded generic block layer. Reads support EHCI USB MSC and read-only ATA PIO. */
bool block_read(const BlockDevice& device, uint64_t lba, uint16_t block_count,
                void* output, uint32_t output_capacity);
bool block_write(const BlockDevice& device, uint32_t lba, uint16_t block_count,
                 const void* input, uint32_t input_capacity);
/* Requires successful SCSI SYNCHRONIZE CACHE(10); unsupported devices return false. */
bool block_flush(const BlockDevice& device);
size_t block_device_count();
bool block_device_get(size_t index, BlockDevice* output);
size_t fat32_volume_count();
bool fat32_volume_get(size_t index, Fat32Volume* output);
size_t rxfs_volume_count();
bool rxfs_volume_get(size_t index, RxfsVolume* output);

/* Scan valid MBR primary partitions or CRC-verified GPT partition tables. */
bool partition_scan(const BlockDevice& device, Partition* output,
                    size_t output_capacity, size_t* output_count);

/* Mount FAT32 only; does not modify disk data. */
bool fat32_mount(const BlockDevice& device, const Partition& partition,
                 Fat32Volume* output);
/* RXFS v1.0 mount and namespace reads are read-only; unsupported features fail closed. */
bool rxfs_mount(const BlockDevice& device, const Partition& partition,
                RxfsVolume* output);
bool rxfs_lookup_path(const RxfsVolume& volume, const char* path,
                      starlix::kernel::rxfs::NativeRxfsNode* output);
bool rxfs_read_file(const RxfsVolume& volume, const char* path,
                    void* output, uint32_t output_capacity, uint32_t* bytes_read);
bool fat32_list_root(const Fat32Volume& volume);
/* Reads a root-directory 8.3 short-name file, e.g. "HELLO.TXT". */
bool fat32_read_file_83(const Fat32Volume& volume, const char* name,
                        void* output, uint32_t output_capacity,
                        uint32_t* bytes_read);
/* Reads a root or nested path, resolves VFAT long names (ASCII and UTF-8 names), read-only. */
bool fat32_read_file(const Fat32Volume& volume, const char* path,
                     void* output, uint32_t output_capacity, uint32_t* bytes_read);
/* Phase 20: overwrite an existing regular file in place; length must remain unchanged.
   Does not create/rename/truncate files or modify FAT/directory metadata. Not transactional:
   a device error may leave some data sectors updated. Caller must serialize storage access. */
bool fat32_overwrite_existing_file(const Fat32Volume& volume, const char* path,
                                   const void* data, uint32_t length);
/* Lists the root for "/" or a named subdirectory such as "Docs". */
bool fat32_list_directory(const Fat32Volume& volume, const char* path);

/* Discovers ATA PIO devices and any ready EHCI MSC devices, then scans partitions. */
void storage_init();

} // namespace starlix::storage
