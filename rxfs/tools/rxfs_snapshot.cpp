// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include "../include/rxfs_format.hpp"

using namespace starlix::rxfs;

static uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
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

static uint64_t alloc_block(std::fstream& fs, RXFS_SUPERBLOCK& sb) {
    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    fs.seekg(sb.block_bitmap_lba * RXFS_BLOCK_SIZE);
    fs.read((char*)bitmap.data(), RXFS_BLOCK_SIZE);

    for (size_t i = 0; i < bitmap.size(); i++) {
        if (bitmap[i] != 0xFF) {
            for (int b = 0; b < 8; b++) {
                if (!(bitmap[i] & (1 << b))) {
                    bitmap[i] |= (1 << b);
                    fs.seekp(sb.block_bitmap_lba * RXFS_BLOCK_SIZE);
                    fs.write((char*)bitmap.data(), RXFS_BLOCK_SIZE);
                    sb.free_blocks--;
                    return sb.data_blocks_lba + (i * 8) + b;
                }
            }
        }
    }
    return 0;
}

static uint64_t alloc_inode(std::fstream& fs, RXFS_SUPERBLOCK& sb) {
    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    fs.seekg(sb.inode_bitmap_lba * RXFS_BLOCK_SIZE);
    fs.read((char*)bitmap.data(), RXFS_BLOCK_SIZE);

    for (size_t i = 0; i < bitmap.size(); i++) {
        if (bitmap[i] != 0xFF) {
            for (int b = 0; b < 8; b++) {
                if (!(bitmap[i] & (1 << b))) {
                    bitmap[i] |= (1 << b);
                    fs.seekp(sb.inode_bitmap_lba * RXFS_BLOCK_SIZE);
                    fs.write((char*)bitmap.data(), RXFS_BLOCK_SIZE);
                    sb.free_inodes--;
                    return (i * 8) + b + 1;
                }
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "Usage:\n  rxfs-snapshot create <volume_image> <snap_name>\n  rxfs-snapshot list   <volume_image>");
        return 1;
    }

    std::string cmd = argv[1];
    std::string imagePath = argv[2];

    std::fstream fs(imagePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!fs) {
        std::println(stderr, "[-] Cannot open volume: {}", imagePath);
        return 1;
    }

    RXFS_SUPERBLOCK sb{};
    fs.seekg(RXFS_BLOCK_SIZE);
    fs.read(reinterpret_cast<char*>(&sb), sizeof(sb));

    if (std::strncmp(sb.magic, "RXFS1QB!", 8) != 0) {
        std::println(stderr, "[-] Invalid RXFS volume!");
        return 1;
    }

    if (cmd == "create") {
        if (argc < 4) { std::println(stderr, "Usage: rxfs-snapshot create <volume_image> <snap_name>"); return 1; }
        std::string snapName = argv[3];

        // 1. Read Root Inode & Root Directory Data Block
        RXFS_INODE rootNode{};
        fs.seekg(sb.inode_table_lba * RXFS_BLOCK_SIZE);
        fs.read(reinterpret_cast<char*>(&rootNode), sizeof(rootNode));

        std::vector<uint8_t> snapDirBlock(RXFS_BLOCK_SIZE, 0);
        fs.seekg(rootNode.direct_blocks[0] * RXFS_BLOCK_SIZE);
        fs.read(reinterpret_cast<char*>(snapDirBlock.data()), RXFS_BLOCK_SIZE);

        // 2. Clone all active file Inodes for Snapshot isolation
        size_t off = 0;
        while (off < RXFS_BLOCK_SIZE) {
            RXFS_DIR_ENTRY* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(&snapDirBlock[off]);
            if (entry->entry_length == 0) break;

            if (entry->inode_number != 0 &&
                std::strcmp(entry->name, ".") != 0 &&
                std::strcmp(entry->name, "..") != 0 &&
                std::strcmp(entry->name, ".snapshots") != 0) {

                // Read original active file inode
                RXFS_INODE origFileInode{};
                fs.seekg((sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((entry->inode_number - 1) * RXFS_INODE_SIZE));
                fs.read(reinterpret_cast<char*>(&origFileInode), sizeof(origFileInode));

                // Allocate dedicated Snapshot File Inode
                uint64_t snapFileInum = alloc_inode(fs, sb);
                RXFS_INODE snapFileInode = origFileInode;
                snapFileInode.inode_number = snapFileInum;
                snapFileInode.flags |= INODE_FLAG_SNAPSHOT;
                snapFileInode.inode_checksum = crc32c(&snapFileInode, sizeof(snapFileInode) - 4);

                // Save cloned snapshot file inode
                fs.seekp((sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((snapFileInum - 1) * RXFS_INODE_SIZE));
                fs.write(reinterpret_cast<const char*>(&snapFileInode), sizeof(snapFileInode));

                // Update entry in snapshot dir block to point to snapFileInum
                entry->inode_number = snapFileInum;
            }
            off += entry->entry_length;
        }

        // 3. Allocate fresh Data Block for Snapshot Directory
        uint64_t snapDirLba = alloc_block(fs, sb);
        fs.seekp(snapDirLba * RXFS_BLOCK_SIZE);
        fs.write(reinterpret_cast<const char*>(snapDirBlock.data()), RXFS_BLOCK_SIZE);

        // 4. Allocate and create Snapshot Root Inode
        uint64_t snapRootInum = alloc_inode(fs, sb);
        RXFS_INODE snapRootNode = rootNode;
        snapRootNode.inode_number = snapRootInum;
        snapRootNode.flags |= INODE_FLAG_SNAPSHOT;
        snapRootNode.direct_blocks[0] = snapDirLba;
        snapRootNode.btime_us = now_us();
        snapRootNode.mtime_us = snapRootNode.btime_us;
        snapRootNode.snapshot_prev = 1;
        std::strncpy(reinterpret_cast<char*>(snapRootNode.tags), snapName.c_str(), sizeof(snapRootNode.tags) - 1);
        snapRootNode.inode_checksum = crc32c(&snapRootNode, sizeof(snapRootNode) - 4);

        fs.seekp((sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((snapRootInum - 1) * RXFS_INODE_SIZE));
        fs.write(reinterpret_cast<const char*>(&snapRootNode), sizeof(snapRootNode));

        // 5. Update Superblock
        sb.last_snapshot_time = now_us();
        sb.superblock_checksum = crc32c(&sb, sizeof(sb) - 4);
        fs.seekp(RXFS_BLOCK_SIZE);
        fs.write(reinterpret_cast<const char*>(&sb), sizeof(sb));

        std::println("============================================================");
        std::println(" RXFS INSTANT COPY-ON-WRITE (COW) SNAPSHOT CREATED");
        std::println("============================================================");
        std::println("[+] Snapshot Name : @{}", snapName);
        std::println("[+] Snapshot Inode: #{}", snapRootNode.inode_number);
        std::println("[+] Creation Time : {} us", snapRootNode.btime_us);
        std::println("[+] COW Isolation : 100% ACTIVE (Dedicated Inode Maps)");
        std::println("============================================================");

    } else if (cmd == "list") {
        std::println("============================================================");
        std::println(" RXFS SNAPSHOT LISTING");
        std::println("============================================================");
        std::println(" Volume: {}", imagePath);
        std::println(" Last Snapshot Timestamp: {} us", sb.last_snapshot_time);
        std::println("");

        bool found = false;
        for (uint64_t i = 1; i <= sb.total_inodes; i++) {
            RXFS_INODE inode{};
            fs.seekg((sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((i - 1) * RXFS_INODE_SIZE));
            if (fs.read(reinterpret_cast<char*>(&inode), sizeof(inode))) {
                if ((inode.flags & INODE_FLAG_SNAPSHOT) && inode.tags[0] != '\0') {
                    std::println("  - Snapshot @{:<20} Inode #{:<5} Size: {} bytes (Created: {} us)",
                                 reinterpret_cast<char*>(inode.tags), inode.inode_number, inode.file_size, inode.btime_us);
                    found = true;
                }
            }
        }
        if (!found) {
            std::println("  (No active snapshots found on volume)");
        }
        std::println("============================================================");
    } else {
        std::println(stderr, "Unknown command: {}", cmd);
        return 1;
    }

    return 0;
}
