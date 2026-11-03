// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include <cstring>
#include "../include/rxfs_format.hpp"

using namespace starlix::rxfs;

static uint64_t now_microseconds() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

static void generate_uuid(uint8_t out[16]) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t* p = reinterpret_cast<uint64_t*>(out);
    p[0] = gen();
    p[1] = gen();
    out[6] = (out[6] & 0x0F) | 0x40;   // UUID v4
    out[8] = (out[8] & 0x3F) | 0x80;   // Variant
}

static uint32_t crc32c(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1));
    }
    return ~crc;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "Usage: mkfs.rxfs <device_or_image> [--size <bytes>] [--label <name>]");
        return 1;
    }

    std::string devicePath = argv[1];
    uint64_t volumeSize = 1024ULL * 1024 * 1024; // Default 1 GB
    std::string label = "STARLIX_USER";

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc) volumeSize = std::stoull(argv[++i]);
        else if (arg == "--label" && i + 1 < argc) label = argv[++i];
    }

    std::println("============================================================");
    std::println(" RXFS v1.0 Formatter (mkfs.rxfs) - C++26 Edition");
    std::println(" OpenRPX xsOft - Starlix Operating System");
    std::println("============================================================");

    {
        std::ofstream prealloc(devicePath, std::ios::binary);
        if (!prealloc) { std::println(stderr, "[-] Cannot create {}", devicePath); return 1; }
    }

    std::fstream fs(devicePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!fs) { std::println(stderr, "[-] Cannot open {}", devicePath); return 1; }

    fs.seekp(volumeSize - 1);
    fs.put(0);
    fs.flush();

    // ---------- Boot Sector (LBA 0) ----------
    RXFS_BOOT_SECTOR boot{};
    boot.jump_boot[0] = 0xEB; boot.jump_boot[1] = 0x58; boot.jump_boot[2] = 0x90;
    std::memcpy(boot.oem_name, "RXFS1QB!", 8);
    boot.sector_size = RXFS_SECTOR_SIZE;
    boot.sectors_per_cluster = (uint8_t)(RXFS_BLOCK_SIZE / RXFS_SECTOR_SIZE);
    boot.reserved_sectors = 8;
    boot.total_sectors = volumeSize / RXFS_SECTOR_SIZE;
    boot.superblock_lba = 1;
    boot.backup_sb_lba = (volumeSize / RXFS_SECTOR_SIZE) - 1;
    generate_uuid(boot.volume_uuid);
    std::strncpy(boot.volume_label, label.c_str(), sizeof(boot.volume_label) - 1);
    boot.boot_signature = 0xAA55;

    fs.seekp(0);
    fs.write(reinterpret_cast<const char*>(&boot), sizeof(boot));

    // ---------- Superblock (LBA 1) ----------
    RXFS_SUPERBLOCK sb{};
    init_superblock(sb, volumeSize);
    std::memcpy(sb.volume_uuid, boot.volume_uuid, 16);
    std::strncpy(sb.volume_label, label.c_str(), sizeof(sb.volume_label) - 1);

    sb.total_inodes = sb.total_blocks / 4;
    sb.inode_bitmap_lba = 2;
    sb.block_bitmap_lba = 10;
    sb.journal_lba = 100;
    sb.journal_size_blocks = 256;
    sb.inode_table_lba = 400;
    sb.data_blocks_lba = sb.inode_table_lba + (sb.total_inodes * RXFS_INODE_SIZE) / RXFS_BLOCK_SIZE;
    sb.free_blocks = sb.total_blocks - sb.data_blocks_lba - 3;
    sb.free_inodes = sb.total_inodes - 3;
    sb.superblock_checksum = crc32c(&sb, sizeof(sb) - 4);

    fs.seekp(RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(&sb), sizeof(sb));

    // ---------- Bitmaps Initialization (CRITICAL FIX) ----------
    std::vector<uint8_t> inodeBitmap(RXFS_BLOCK_SIZE, 0);
    inodeBitmap[0] = 0x07; // Mark Inodes #1, #2, #3 as USED (bits 0, 1, 2)
    fs.seekp(sb.inode_bitmap_lba * RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(inodeBitmap.data()), inodeBitmap.size());

    std::vector<uint8_t> blockBitmap(RXFS_BLOCK_SIZE, 0);
    blockBitmap[0] = 0x07; // Mark Data Blocks #0, #1, #2 as USED (bits 0, 1, 2)
    fs.seekp(sb.block_bitmap_lba * RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(blockBitmap.data()), blockBitmap.size());

    // ---------- Populate Sample Files ----------
    std::string welcomeContent =
        "Welcome to RXFS (Rapid eXtensible File System) v1.0!\n"
        "Created by OpenRPX xsOft for Starlix OS.\n"
        "Features: 1 QB Max Volume, BLAKE3, Zstd, AES-256-XTS, COW Snapshots.\n";

    std::string starlixSysContent =
        "STARLIX_OS_VERSION=1.0.Beta\n"
        "BUILD_COMPANY=OpenRPX_xsOft\n"
        "FS_TYPE=RXFS1QB!\n"
        "SECURITY_LEVEL=HIGH\n";

    uint64_t welcomeDataLba = sb.data_blocks_lba;
    uint64_t sysDataLba     = sb.data_blocks_lba + 1;

    fs.seekp(welcomeDataLba * RXFS_BLOCK_SIZE);
    fs.write(welcomeContent.c_str(), welcomeContent.size());

    fs.seekp(sysDataLba * RXFS_BLOCK_SIZE);
    fs.write(starlixSysContent.c_str(), starlixSysContent.size());

    // Inode #2: /welcome.txt
    RXFS_INODE inodeWelcome{};
    inodeWelcome.inode_number = 2;
    inodeWelcome.file_type = RXFS_TYPE_REGULAR;
    inodeWelcome.permissions = 0644;
    inodeWelcome.file_size = welcomeContent.size();
    inodeWelcome.allocated_blocks = 1;
    inodeWelcome.link_count = 1;
    inodeWelcome.direct_blocks[0] = welcomeDataLba;
    inodeWelcome.btime_us = now_microseconds();
    inodeWelcome.mtime_us = inodeWelcome.btime_us;
    inodeWelcome.inode_checksum = crc32c(&inodeWelcome, sizeof(inodeWelcome) - 4);

    // Inode #3: /starlix.sys
    RXFS_INODE inodeSys{};
    inodeSys.inode_number = 3;
    inodeSys.file_type = RXFS_TYPE_REGULAR;
    inodeSys.permissions = 0644;
    inodeSys.file_size = starlixSysContent.size();
    inodeSys.allocated_blocks = 1;
    inodeSys.link_count = 1;
    inodeSys.direct_blocks[0] = sysDataLba;
    inodeSys.btime_us = now_microseconds();
    inodeSys.mtime_us = inodeSys.btime_us;
    inodeSys.inode_checksum = crc32c(&inodeSys, sizeof(inodeSys) - 4);

    fs.seekp((sb.inode_table_lba * RXFS_BLOCK_SIZE) + RXFS_INODE_SIZE);
    fs.write(reinterpret_cast<const char*>(&inodeWelcome), sizeof(inodeWelcome));

    fs.seekp((sb.inode_table_lba * RXFS_BLOCK_SIZE) + (2 * RXFS_INODE_SIZE));
    fs.write(reinterpret_cast<const char*>(&inodeSys), sizeof(inodeSys));

    // ---------- Root Inode (#1 Directory) ----------
    uint64_t rootDirLba = sb.data_blocks_lba + 2;
    std::vector<uint8_t> dirBlock(RXFS_BLOCK_SIZE, 0);

    size_t dirOffset = 0;
    auto add_dir_entry = [&](uint64_t inum, uint8_t ftype, const std::string& fname) {
        RXFS_DIR_ENTRY* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(&dirBlock[dirOffset]);
        entry->inode_number = inum;
        entry->file_type = ftype;
        entry->name_length = fname.size();
        uint16_t elen = sizeof(RXFS_DIR_ENTRY) + fname.size() + 1;
        elen = (elen + 7) & ~7;
        entry->entry_length = elen;
        std::memcpy(entry->name, fname.c_str(), fname.size() + 1);
        dirOffset += elen;
    };

    add_dir_entry(1, RXFS_TYPE_DIRECTORY, ".");
    add_dir_entry(1, RXFS_TYPE_DIRECTORY, "..");
    add_dir_entry(2, RXFS_TYPE_REGULAR,   "welcome.txt");
    add_dir_entry(3, RXFS_TYPE_REGULAR,   "starlix.sys");

    fs.seekp(rootDirLba * RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(dirBlock.data()), dirBlock.size());

    RXFS_INODE root{};
    root.inode_number = 1;
    root.file_type = RXFS_TYPE_DIRECTORY;
    root.permissions = 0755;
    root.link_count = 2;
    root.file_size = dirOffset;
    root.allocated_blocks = 1;
    root.direct_blocks[0] = rootDirLba;
    root.btime_us = now_microseconds();
    root.mtime_us = root.btime_us;
    root.inode_checksum = crc32c(&root, sizeof(root) - 4);

    fs.seekp(sb.inode_table_lba * RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(&root), sizeof(root));

    // Backup Superblock
    fs.seekp(volumeSize - RXFS_BLOCK_SIZE);
    fs.write(reinterpret_cast<const char*>(&sb), sizeof(sb));

    fs.close();

    std::println("[+] Formatted & Populated Sample Files (/welcome.txt, /starlix.sys)");
    std::println("[+] Bitmaps Properly Initialized!");
    return 0;
}
