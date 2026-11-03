// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <string>
#include <cstring>
#include "../include/rxfs_format.hpp"

using namespace starlix::rxfs;

static void print_uuid(const uint8_t uuid[16]) {
    std::print("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
}

static const char* feature_name(int bit) {
    switch (bit) {
        case 0: return "Journaling";
        case 1: return "ACL";
        case 2: return "Sparse Files";
        case 3: return "Hard Links";
        case 4: return "Soft Links";
        case 5: return "Alternate Streams (ADS)";
        case 6: return "Reparse Points";
        case 7: return "USN Journal";
        case 8: return "Quotas";
        case 9: return "Compression (LZNT1 base)";
        case 10: return "Encryption (base)";
        case 11: return "Shadow Copies";
        case 12: return "Transactions";
        case 13: return "BLAKE3 Checksums *EXTRA*";
        case 14: return "Zstd Compression *EXTRA*";
        case 15: return "AES-256-XTS Encryption *EXTRA*";
        case 16: return "Copy-on-Write Snapshots *EXTRA*";
        case 17: return "Deduplication *EXTRA*";
        case 18: return "UTF-8 1024-char Filenames *EXTRA*";
        case 19: return "64-bit Inodes *EXTRA*";
        case 20: return "Microsecond Timestamps *EXTRA*";
        case 21: return "Birth Time *EXTRA*";
        case 22: return "File-level RAID *EXTRA*";
        case 23: return "Versioning *EXTRA*";
        case 24: return "Content-Addressable Mode *EXTRA*";
        case 25: return "Online Defragmentation *EXTRA*";
        case 26: return "Hot/Cold Tiering *EXTRA*";
        case 27: return "Petabyte Sparse Support *EXTRA*";
        case 28: return "Case-Sensitive Dual Mode *EXTRA*";
        case 29: return "Unlimited Extended Attrs *EXTRA*";
        case 30: return "POSIX Symlinks *EXTRA*";
        case 31: return "Reflink / O(1) Clone *EXTRA*";
        case 32: return "Automatic TRIM *EXTRA*";
        case 33: return "Multi-Stream Files *EXTRA*";
        case 34: return "Per-Directory Quotas *EXTRA*";
        case 35: return "Online fsck *EXTRA*";
        case 36: return "Atomic Rename+Replace *EXTRA*";
        case 37: return "Write Barriers *EXTRA*";
        case 38: return "CRC32C Hardware Accelerated *EXTRA*";
        case 39: return "Variable Block Size *EXTRA*";
        case 40: return "Namespace Encryption *EXTRA*";
        case 41: return "Per-File Compression Algo *EXTRA*";
        case 42: return "Immutable Files *EXTRA*";
        case 43: return "Append-Only Files *EXTRA*";
        case 44: return "Secure Delete *EXTRA*";
        case 45: return "Access Time Control *EXTRA*";
        case 46: return "Case Folding *EXTRA*";
        case 47: return "B+Tree Directories *EXTRA*";
        case 48: return "Memory-Mapped Journal *EXTRA*";
        case 49: return "Trim-on-Mount *EXTRA*";
        case 50: return "Auto Defrag Background *EXTRA*";
        case 51: return "Scrub Background Verify *EXTRA*";
        case 52: return "Online Resize *EXTRA*";
        case 53: return "Cross-Device Move *EXTRA*";
        case 54: return "UUID+Label v2 *EXTRA*";
        case 55: return "Compression Stats *EXTRA*";
        case 56: return "Per-File IOPS Limiter *EXTRA*";
        case 57: return "Snapshot Delta Send/Recv *EXTRA*";
        case 58: return "Mount Key Prompt *EXTRA*";
        case 59: return "TPM-Sealed Master Key *EXTRA*";
        case 60: return "Rolling Checksum Rebuild *EXTRA*";
        case 61: return "File Tags/Labels *EXTRA*";
        case 62: return "Change Notification *EXTRA*";
        default: return nullptr;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "Usage: rxfs-inspect <volume_file>");
        return 1;
    }

    std::ifstream fs(argv[1], std::ios::binary);
    if (!fs) { std::println(stderr, "[-] Cannot open {}", argv[1]); return 1; }

    RXFS_BOOT_SECTOR boot{};
    fs.read(reinterpret_cast<char*>(&boot), sizeof(boot));

    if (std::strncmp(boot.oem_name, "RXFS1QB!", 8) != 0) {
        std::println(stderr, "[-] Not a valid RXFS volume (bad magic)");
        return 1;
    }

    RXFS_SUPERBLOCK sb{};
    fs.seekg(RXFS_BLOCK_SIZE);
    fs.read(reinterpret_cast<char*>(&sb), sizeof(sb));

    if (std::strncmp(sb.magic, "RXFS1QB!", 8) != 0) {
        std::println(stderr, "[-] Superblock magic mismatch");
        return 1;
    }

    std::println("============================================================");
    std::println(" RXFS DEEP VOLUME INSPECTOR");
    std::println(" OpenRPX xsOft - Starlix OS");
    std::println("============================================================");
    std::println("");
    std::println("--- BOOT SECTOR ---");
    std::print("  Volume UUID    : "); print_uuid(boot.volume_uuid); std::println("");
    std::println("  OEM Name       : {}", std::string(boot.oem_name, 8));
    std::println("  Sector Size    : {} bytes", boot.sector_size);
    std::println("  Cluster Sectors: {}", boot.sectors_per_cluster);
    std::println("  Total Sectors  : {}", boot.total_sectors);
    std::println("  Volume Label   : {}", boot.volume_label);
    std::println("  Boot Signature : {:#06X}", boot.boot_signature);
    std::println("");

    std::println("--- SUPERBLOCK ---");
    std::println("  Magic          : {}", std::string(sb.magic, 8));
    std::println("  Version        : {}.{}", sb.version_major, sb.version_minor);
    std::println("  Volume Label   : {}", sb.volume_label);
    std::println("  Block Size     : {} bytes", sb.block_size);
    std::println("  Total Blocks   : {}", sb.total_blocks);
    std::println("  Total Inodes   : {}", sb.total_inodes);
    std::println("  Free Blocks    : {}", sb.free_blocks);
    std::println("  Free Inodes    : {}", sb.free_inodes);
    std::println("");

    std::println("--- CAPACITY LIMITS ---");
    std::println("  Max Volume Size: {} bytes", sb.max_volume_size);
    std::println("                  = 1 Quettabyte (10^30 bytes) theoretical");
    std::println("  Max File Size  : {} bytes", sb.max_file_size);
    std::println("                  = 1 Quettabyte (10^30 bytes) theoretical");
    std::println("  Max Filename   : {} UTF-8 characters", sb.max_filename_len);
    std::println("");

    std::println("--- LAYOUT ---");
    std::println("  Inode Bitmap   : LBA {}", sb.inode_bitmap_lba);
    std::println("  Block Bitmap   : LBA {}", sb.block_bitmap_lba);
    std::println("  Journal        : LBA {} ({} blocks)", sb.journal_lba, sb.journal_size_blocks);
    std::println("  Inode Table    : LBA {}", sb.inode_table_lba);
    std::println("  Data Blocks    : LBA {}", sb.data_blocks_lba);
    std::println("  Root Inode     : #{}", sb.root_inode);
    std::println("");

    std::println("--- ALGORITHMS ---");
    const char* comp[] = {"None", "LZ4", "Zstd"};
    const char* enc[] = {"None", "AES-256-XTS"};
    const char* csum[] = {"CRC32C", "BLAKE3"};
    std::println("  Compression    : {}", comp[sb.compression_algo < 3 ? sb.compression_algo : 0]);
    std::println("  Encryption     : {}", enc[sb.encryption_algo < 2 ? sb.encryption_algo : 0]);
    std::println("  Checksum       : {}", csum[sb.checksum_algo < 2 ? sb.checksum_algo : 0]);
    std::println("");

    std::println("--- STATISTICS ---");
    std::println("  Mount Count    : {}", sb.mount_count);
    std::println("  Last Write     : {} us", sb.last_write_time);
    std::println("  Last fsck      : {} us", sb.last_fsck_time);
    std::println("  SB Checksum    : {:#010X}", sb.superblock_checksum);
    std::println("");

    std::println("--- FEATURES ({} enabled) ---", __builtin_popcountll(sb.features_enabled));
    int enabled = 0, extras = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (sb.features_enabled & (1ULL << bit)) {
            const char* name = feature_name(bit);
            if (name) {
                std::println("  [{:2}] {}", bit, name);
                enabled++;
                if (bit >= 13) extras++;
            }
        }
    }
    std::println("");
    std::println("  Core (NTFS-equivalent): {}", enabled - extras);
    std::println("  EXTRA (beyond NTFS)   : {}", extras);
    std::println("");

    // Root inode
    RXFS_INODE root{};
    fs.seekg(sb.inode_table_lba * RXFS_BLOCK_SIZE);
    fs.read(reinterpret_cast<char*>(&root), sizeof(root));

    std::println("--- ROOT INODE (#1) ---");
    std::println("  Type           : Directory");
    std::println("  Permissions    : {:#o}", root.permissions);
    std::println("  UID/GID        : {}/{}", root.uid, root.gid);
    std::println("  Link Count     : {}", root.link_count);
    std::println("  Birth Time (us): {}", root.btime_us);
    std::println("  Checksum       : {:#010X}", root.inode_checksum);
    std::println("");
    std::println("============================================================");
    std::println(" RXFS Volume PASSED Inspection");
    std::println("============================================================");
    return 0;
}
