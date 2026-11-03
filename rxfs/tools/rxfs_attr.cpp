// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include "../include/rxfs_format.hpp"

using namespace starlix::rxfs;

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

static uint64_t lookup_in_dir(FILE* f, const RXFS_SUPERBLOCK& sb, uint64_t parentInum, const std::string& name) {
    uint64_t pOffset = (sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((parentInum - 1) * RXFS_INODE_SIZE);
    std::fseek(f, pOffset, SEEK_SET);
    RXFS_INODE parent{};
    if (std::fread(&parent, sizeof(parent), 1, f) != 1) return 0;

    std::vector<uint8_t> dirBuf(RXFS_BLOCK_SIZE);
    std::fseek(f, parent.direct_blocks[0] * RXFS_BLOCK_SIZE, SEEK_SET);
    if (std::fread(dirBuf.data(), RXFS_BLOCK_SIZE, 1, f) != 1) return 0;

    size_t offset = 0;
    while (offset < dirBuf.size()) {
        const RXFS_DIR_ENTRY* entry = reinterpret_cast<const RXFS_DIR_ENTRY*>(&dirBuf[offset]);
        if (entry->entry_length == 0) break;
        if (entry->inode_number != 0 && std::strcmp(entry->name, name.c_str()) == 0) {
            return entry->inode_number;
        }
        offset += entry->entry_length;
    }
    return 0;
}

static uint64_t resolve_path(FILE* f, const RXFS_SUPERBLOCK& sb, const std::string& path) {
    if (path.empty() || path[0] != '/') return 0;
    if (path == "/") return 1;

    std::vector<std::string> parts;
    std::string curr;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            if (!curr.empty()) { parts.push_back(curr); curr.clear(); }
        } else {
            curr.push_back(path[i]);
        }
    }
    if (!curr.empty()) parts.push_back(curr);

    uint64_t currInum = 1;
    for (const auto& part : parts) {
        currInum = lookup_in_dir(f, sb, currInum, part);
        if (currInum == 0) return 0;
    }
    return currInum;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::println(stderr, "Usage:\n  rxfs-attr get <volume_image> <file_path>\n  rxfs-attr set <volume_image> <file_path> [+i|-i] [+a|-a]");
        return 1;
    }

    std::string action = argv[1];
    std::string imagePath = argv[2];
    std::string filePath = argv[3];

    FILE* f = std::fopen(imagePath.c_str(), "r+b");
    if (!f) {
        std::println(stderr, "[-] Cannot open image: {}", imagePath);
        return 1;
    }

    RXFS_SUPERBLOCK sb{};
    std::fseek(f, RXFS_BLOCK_SIZE, SEEK_SET);
    if (std::fread(&sb, sizeof(sb), 1, f) != 1 || std::strncmp(sb.magic, "RXFS1QB!", 8) != 0) {
        std::println(stderr, "[-] Invalid RXFS volume!");
        std::fclose(f);
        return 1;
    }

    uint64_t inum = resolve_path(f, sb, filePath);
    if (inum == 0) {
        std::println(stderr, "[-] File not found: {}", filePath);
        std::fclose(f);
        return 1;
    }

    uint64_t offset = (sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((inum - 1) * RXFS_INODE_SIZE);
    std::fseek(f, offset, SEEK_SET);
    RXFS_INODE inode{};
    std::fread(&inode, sizeof(inode), 1, f);

    if (action == "get") {
        std::println("============================================================");
        std::println(" RXFS FILE ATTRIBUTE INSPECTOR");
        std::println("============================================================");
        std::println("  File Path : {}", filePath);
        std::println("  Inode     : #{}", inode.inode_number);
        std::println("  Flags Hex : {:#010X}", inode.flags);
        std::println("  Immutable : {}", (inode.flags & INODE_FLAG_IMMUTABLE) ? "YES (+i)" : "NO");
        std::println("  AppendOnly: {}", (inode.flags & INODE_FLAG_APPEND_ONLY) ? "YES (+a)" : "NO");
        std::println("  Compressed: {}", (inode.flags & INODE_FLAG_COMPRESSED) ? "YES" : "NO");
        std::println("  Encrypted : {}", (inode.flags & INODE_FLAG_ENCRYPTED) ? "YES" : "NO");
        std::println("============================================================");

    } else if (action == "set") {
        if (argc < 5) {
            std::println(stderr, "Usage: rxfs-attr set <volume_image> <file_path> [+i|-i] [+a|-a]");
            std::fclose(f);
            return 1;
        }

        for (int i = 4; i < argc; i++) {
            std::string attr = argv[i];
            if (attr == "+i") inode.flags |= INODE_FLAG_IMMUTABLE;
            else if (attr == "-i") inode.flags &= ~INODE_FLAG_IMMUTABLE;
            else if (attr == "+a") inode.flags |= INODE_FLAG_APPEND_ONLY;
            else if (attr == "-a") inode.flags &= ~INODE_FLAG_APPEND_ONLY;
        }

        inode.inode_checksum = crc32c(&inode, sizeof(inode) - 4);
        std::fseek(f, offset, SEEK_SET);
        std::fwrite(&inode, sizeof(inode), 1, f);
        std::fflush(f);

        std::println("[+] Updated Attributes for '{}': Flags = {:#010X}", filePath, inode.flags);
    } else {
        std::println(stderr, "[-] Unknown action: {}", action);
    }

    std::fclose(f);
    return 0;
}
