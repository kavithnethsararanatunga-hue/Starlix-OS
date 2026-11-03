// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <zstd.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <print>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <sys/stat.h>
#include "../include/rxfs_format.hpp"

using namespace starlix::rxfs;

constexpr uint32_t DIRECT_BLOCKS       = 12;
constexpr uint64_t POINTERS_PER_BLOCK  = RXFS_BLOCK_SIZE / sizeof(uint64_t);
constexpr uint64_t SINGLE_INDIRECT_CAP = POINTERS_PER_BLOCK;

constexpr uint32_t ZSTD_BLOCK_MAGIC    = 0x5A535444;
constexpr uint32_t JOURNAL_MAGIC       = 0x52584A4E;

static const uint8_t RXFS_AES_XTS_MASTER_KEY[64] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, 0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17, 0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27, 0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37, 0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F
};

#pragma pack(push, 1)
struct ZstdBlockHeader {
    uint32_t magic;
    uint32_t comp_size;
    uint32_t orig_size;
    uint32_t crc32;
    uint8_t  block_sha256[32];
};

struct RXFS_DEDUP_ENTRY {
    uint32_t hash_crc32;
    uint64_t physical_lba;
    uint32_t ref_count;
};
#pragma pack(pop)

struct DedupInfo {
    uint64_t lba;
    uint32_t ref_count;
};

struct RxfsState {
    std::string imagePath;
    FILE* devFile;
    RXFS_SUPERBLOCK sb;
    std::unordered_map<uint32_t, DedupInfo> dedupMap;
    uint32_t journalSeq;
};

static RxfsState g_state;

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

static bool compute_sha256(const void* data, size_t len, uint8_t outHash[32]) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, outHash, NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    return true;
}

static bool aes_256_xts_crypt(const uint8_t* in, uint8_t* out, size_t len, uint64_t lba, bool doEncrypt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    uint8_t iv[16] = {0};
    std::memcpy(iv, &lba, sizeof(lba));

    int res = EVP_CipherInit_ex(ctx, EVP_aes_256_xts(), NULL, RXFS_AES_XTS_MASTER_KEY, iv, doEncrypt ? 1 : 0);
    if (res != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int outLen = 0;
    res = EVP_CipherUpdate(ctx, out, &outLen, in, (int)len);
    if (res != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int finalLen = 0;
    res = EVP_CipherFinal_ex(ctx, out + outLen, &finalLen);
    EVP_CIPHER_CTX_free(ctx);
    return res == 1;
}

// ---------------- Low-Level Disk I/O ----------------

static bool read_block(uint64_t lba, void* buffer) {
    std::fseek(g_state.devFile, lba * RXFS_BLOCK_SIZE, SEEK_SET);
    return std::fread(buffer, RXFS_BLOCK_SIZE, 1, g_state.devFile) == 1;
}

static bool write_block(uint64_t lba, const void* buffer) {
    std::fseek(g_state.devFile, lba * RXFS_BLOCK_SIZE, SEEK_SET);
    bool ok = std::fwrite(buffer, RXFS_BLOCK_SIZE, 1, g_state.devFile) == 1;
    std::fflush(g_state.devFile);
    return ok;
}

// ---------------- Journaling WAL Engine ----------------

static void log_journal_tx(uint32_t opType, uint64_t targetLba, const void* payload) {
    if (g_state.sb.journal_lba == 0) return;

    uint32_t slot = g_state.journalSeq % g_state.sb.journal_size_blocks;
    uint64_t jLba = g_state.sb.journal_lba + slot;

    std::vector<uint8_t> jBlock(RXFS_BLOCK_SIZE, 0);
    RXFS_JOURNAL_HEADER* jhdr = reinterpret_cast<RXFS_JOURNAL_HEADER*>(jBlock.data());
    jhdr->magic = JOURNAL_MAGIC;
    jhdr->sequence = ++g_state.journalSeq;
    jhdr->op_type = opType;
    jhdr->data_length = RXFS_BLOCK_SIZE - sizeof(RXFS_JOURNAL_HEADER);
    jhdr->timestamp_us = now_us();
    jhdr->target_lba = targetLba;

    if (payload) {
        std::memcpy(jBlock.data() + sizeof(RXFS_JOURNAL_HEADER), payload, jhdr->data_length);
    }
    jhdr->checksum = crc32c(jBlock.data(), RXFS_BLOCK_SIZE - 4);

    write_block(jLba, jBlock.data());
}

static void replay_journal_recovery() {
    if (g_state.sb.journal_lba == 0) return;

    size_t replayed = 0;
    for (uint64_t i = 0; i < g_state.sb.journal_size_blocks; i++) {
        uint64_t jLba = g_state.sb.journal_lba + i;
        std::vector<uint8_t> jBlock(RXFS_BLOCK_SIZE);
        if (!read_block(jLba, jBlock.data())) continue;

        const RXFS_JOURNAL_HEADER* jhdr = reinterpret_cast<const RXFS_JOURNAL_HEADER*>(jBlock.data());
        if (jhdr->magic == JOURNAL_MAGIC) {
            uint32_t csum = crc32c(jBlock.data(), RXFS_BLOCK_SIZE - 4);
            if (csum == jhdr->checksum && jhdr->target_lba > 0) {
                write_block(jhdr->target_lba, jBlock.data() + sizeof(RXFS_JOURNAL_HEADER));
                replayed++;
            }
        }
    }
    std::println("[+] [RXFS JOURNAL RECOVERY] Scanned Journal & Replayed {} Transactions to Disk", replayed);
}

// ---------------- Inode I/O ----------------

static bool read_inode(uint64_t inum, RXFS_INODE& out) {
    if (inum == 0 || inum > g_state.sb.total_inodes) return false;
    uint64_t offset = (g_state.sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((inum - 1) * RXFS_INODE_SIZE);
    std::fseek(g_state.devFile, offset, SEEK_SET);
    return std::fread(&out, sizeof(out), 1, g_state.devFile) == 1;
}

static bool write_inode(uint64_t inum, RXFS_INODE& inode) {
    if (inum == 0 || inum > g_state.sb.total_inodes) return false;
    inode.inode_checksum = crc32c(&inode, sizeof(inode) - 4);
    uint64_t offset = (g_state.sb.inode_table_lba * RXFS_BLOCK_SIZE) + ((inum - 1) * RXFS_INODE_SIZE);

    log_journal_tx(JOP_UPDATE_INODE, (offset / RXFS_BLOCK_SIZE), &inode);

    std::fseek(g_state.devFile, offset, SEEK_SET);
    bool ok = std::fwrite(&inode, sizeof(inode), 1, g_state.devFile) == 1;
    std::fflush(g_state.devFile);
    return ok;
}

static void update_superblock() {
    g_state.sb.superblock_checksum = crc32c(&g_state.sb, sizeof(g_state.sb) - 4);
    std::fseek(g_state.devFile, RXFS_BLOCK_SIZE, SEEK_SET);
    std::fwrite(&g_state.sb, sizeof(g_state.sb), 1, g_state.devFile);
    std::fflush(g_state.devFile);
}

// ---------------- On-Disk Dedup Table Sync ----------------

static void load_dedup_table() {
    g_state.dedupMap.clear();
    if (g_state.sb.dedup_table_lba == 0) return;

    std::vector<uint8_t> blockBuf(RXFS_BLOCK_SIZE);
    if (!read_block(g_state.sb.dedup_table_lba, blockBuf.data())) return;

    size_t maxEntries = RXFS_BLOCK_SIZE / sizeof(RXFS_DEDUP_ENTRY);
    const RXFS_DEDUP_ENTRY* entries = reinterpret_cast<const RXFS_DEDUP_ENTRY*>(blockBuf.data());

    for (size_t i = 0; i < maxEntries; i++) {
        if (entries[i].hash_crc32 != 0 && entries[i].physical_lba != 0) {
            g_state.dedupMap[entries[i].hash_crc32] = {entries[i].physical_lba, entries[i].ref_count};
        }
    }
}

static void save_dedup_table() {
    if (g_state.sb.dedup_table_lba == 0) return;

    std::vector<uint8_t> blockBuf(RXFS_BLOCK_SIZE, 0);
    RXFS_DEDUP_ENTRY* entries = reinterpret_cast<RXFS_DEDUP_ENTRY*>(blockBuf.data());
    size_t idx = 0;
    size_t maxEntries = RXFS_BLOCK_SIZE / sizeof(RXFS_DEDUP_ENTRY);

    for (const auto& [hash, info] : g_state.dedupMap) {
        if (idx >= maxEntries) break;
        entries[idx].hash_crc32 = hash;
        entries[idx].physical_lba = info.lba;
        entries[idx].ref_count = info.ref_count;
        idx++;
    }

    write_block(g_state.sb.dedup_table_lba, blockBuf.data());
}

// ---------------- Bitmap Allocator ----------------

static uint64_t allocate_inode() {
    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    read_block(g_state.sb.inode_bitmap_lba, bitmap.data());

    for (size_t byteIdx = 0; byteIdx < bitmap.size(); byteIdx++) {
        if (bitmap[byteIdx] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap[byteIdx] & (1 << bit))) {
                    bitmap[byteIdx] |= (1 << bit);
                    log_journal_tx(JOP_ALLOC_BLOCK, g_state.sb.inode_bitmap_lba, bitmap.data());
                    write_block(g_state.sb.inode_bitmap_lba, bitmap.data());
                    uint64_t inum = (byteIdx * 8) + bit + 1;
                    g_state.sb.free_inodes--;
                    update_superblock();
                    return inum;
                }
            }
        }
    }
    return 0;
}

static uint64_t allocate_data_block() {
    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    read_block(g_state.sb.block_bitmap_lba, bitmap.data());

    for (size_t byteIdx = 0; byteIdx < bitmap.size(); byteIdx++) {
        if (bitmap[byteIdx] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap[byteIdx] & (1 << bit))) {
                    bitmap[byteIdx] |= (1 << bit);
                    log_journal_tx(JOP_ALLOC_BLOCK, g_state.sb.block_bitmap_lba, bitmap.data());
                    write_block(g_state.sb.block_bitmap_lba, bitmap.data());
                    uint64_t lba = g_state.sb.data_blocks_lba + (byteIdx * 8) + bit;
                    g_state.sb.free_blocks--;
                    update_superblock();
                    return lba;
                }
            }
        }
    }
    return 0;
}

static uint64_t allocate_or_dedup_block(const void* dataBuf, bool& isDeduped) {
    isDeduped = false;
    uint32_t blockHash = crc32c(dataBuf, RXFS_BLOCK_SIZE);

    auto it = g_state.dedupMap.find(blockHash);
    if (it != g_state.dedupMap.end()) {
        isDeduped = true;
        it->second.ref_count++;
        save_dedup_table();
        return it->second.lba;
    }

    uint64_t newLba = allocate_data_block();
    if (newLba == 0) return 0;

    g_state.dedupMap[blockHash] = {newLba, 1};
    save_dedup_table();
    return newLba;
}

static void free_inode_num(uint64_t inum) {
    if (inum == 0) return;
    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    read_block(g_state.sb.inode_bitmap_lba, bitmap.data());
    uint64_t idx = inum - 1;
    bitmap[idx / 8] &= ~(1 << (idx % 8));
    log_journal_tx(JOP_FREE_BLOCK, g_state.sb.inode_bitmap_lba, bitmap.data());
    write_block(g_state.sb.inode_bitmap_lba, bitmap.data());
    g_state.sb.free_inodes++;
    update_superblock();
}

static void free_data_block(uint64_t lba) {
    if (lba < g_state.sb.data_blocks_lba) return;

    for (auto it = g_state.dedupMap.begin(); it != g_state.dedupMap.end(); ++it) {
        if (it->second.lba == lba) {
            if (it->second.ref_count > 1) {
                it->second.ref_count--;
                save_dedup_table();
                return;
            } else {
                g_state.dedupMap.erase(it);
                save_dedup_table();
                break;
            }
        }
    }

    std::vector<uint8_t> bitmap(RXFS_BLOCK_SIZE);
    read_block(g_state.sb.block_bitmap_lba, bitmap.data());
    uint64_t idx = lba - g_state.sb.data_blocks_lba;
    bitmap[idx / 8] &= ~(1 << (idx % 8));
    log_journal_tx(JOP_FREE_BLOCK, g_state.sb.block_bitmap_lba, bitmap.data());
    write_block(g_state.sb.block_bitmap_lba, bitmap.data());
    g_state.sb.free_blocks++;
    update_superblock();
}

// ---------------- Block Resolution & Pointer Allocation ----------------

static uint64_t get_file_block_lba(const RXFS_INODE& inode, uint64_t logical_idx) {
    if (logical_idx < DIRECT_BLOCKS) {
        return inode.direct_blocks[logical_idx];
    }
    logical_idx -= DIRECT_BLOCKS;

    if (logical_idx < SINGLE_INDIRECT_CAP) {
        if (inode.indirect_block == 0) return 0;
        std::vector<uint64_t> indirect(POINTERS_PER_BLOCK);
        read_block(inode.indirect_block, indirect.data());
        return indirect[logical_idx];
    }
    return 0;
}

static uint64_t allocate_file_block(RXFS_INODE& inode, uint64_t logical_idx) {
    if (logical_idx < DIRECT_BLOCKS) {
        if (inode.direct_blocks[logical_idx] == 0) {
            uint64_t newLba = allocate_data_block();
            if (newLba == 0) return 0;
            inode.direct_blocks[logical_idx] = newLba;
            inode.allocated_blocks++;
            std::vector<uint8_t> zero(RXFS_BLOCK_SIZE, 0);
            write_block(newLba, zero.data());
        }
        return inode.direct_blocks[logical_idx];
    }
    logical_idx -= DIRECT_BLOCKS;

    if (logical_idx < SINGLE_INDIRECT_CAP) {
        if (inode.indirect_block == 0) {
            uint64_t newIndirect = allocate_data_block();
            if (newIndirect == 0) return 0;
            inode.indirect_block = newIndirect;
            inode.allocated_blocks++;
            std::vector<uint8_t> zero(RXFS_BLOCK_SIZE, 0);
            write_block(newIndirect, zero.data());
        }
        std::vector<uint64_t> indirect(POINTERS_PER_BLOCK);
        read_block(inode.indirect_block, indirect.data());

        if (indirect[logical_idx] == 0) {
            uint64_t newLba = allocate_data_block();
            if (newLba == 0) return 0;
            indirect[logical_idx] = newLba;
            inode.allocated_blocks++;
            std::vector<uint8_t> zero(RXFS_BLOCK_SIZE, 0);
            write_block(newLba, zero.data());
            write_block(inode.indirect_block, indirect.data());
        }
        return indirect[logical_idx];
    }
    return 0;
}

static void free_all_file_blocks(RXFS_INODE& inode) {
    for (uint32_t i = 0; i < DIRECT_BLOCKS; i++) {
        if (inode.direct_blocks[i] != 0) {
            free_data_block(inode.direct_blocks[i]);
            inode.direct_blocks[i] = 0;
        }
    }
    if (inode.indirect_block != 0) {
        std::vector<uint64_t> indirect(POINTERS_PER_BLOCK);
        read_block(inode.indirect_block, indirect.data());
        for (uint32_t i = 0; i < POINTERS_PER_BLOCK; i++) {
            if (indirect[i] != 0) free_data_block(indirect[i]);
        }
        free_data_block(inode.indirect_block);
        inode.indirect_block = 0;
    }
    inode.allocated_blocks = 0;
}

// ---------------- Path Resolution & Virtual /.snapshots/ Engine ----------------

static uint64_t lookup_in_dir(uint64_t parentInum, const std::string& name) {
    RXFS_INODE parent{};
    if (!read_inode(parentInum, parent)) return 0;

    uint64_t totalBlocks = (parent.file_size + RXFS_BLOCK_SIZE - 1) / RXFS_BLOCK_SIZE;
    if (totalBlocks == 0) totalBlocks = 1;

    for (uint64_t bIdx = 0; bIdx < totalBlocks; bIdx++) {
        uint64_t lba = get_file_block_lba(parent, bIdx);
        if (lba == 0) continue;

        std::vector<uint8_t> dirBuf(RXFS_BLOCK_SIZE);
        if (!read_block(lba, dirBuf.data())) continue;

        size_t offset = 0;
        while (offset < dirBuf.size()) {
            const RXFS_DIR_ENTRY* entry = reinterpret_cast<const RXFS_DIR_ENTRY*>(&dirBuf[offset]);
            if (entry->entry_length == 0) break;
            if (entry->inode_number != 0 && std::strcmp(entry->name, name.c_str()) == 0) {
                return entry->inode_number;
            }
            offset += entry->entry_length;
        }
    }
    return 0;
}

static uint64_t find_snapshot_root_by_name(const std::string& snapName) {
    for (uint64_t i = 1; i <= g_state.sb.total_inodes; i++) {
        RXFS_INODE inode{};
        if (read_inode(i, inode)) {
            if (inode.flags & INODE_FLAG_SNAPSHOT) {
                if (std::strcmp(reinterpret_cast<const char*>(inode.tags), snapName.c_str()) == 0) {
                    return inode.inode_number;
                }
            }
        }
    }
    return 0;
}

static bool resolve_path_from(const std::string& path, uint64_t startInum, uint64_t& parentInum, std::string& childName, uint64_t& targetInum) {
    parentInum = startInum;
    targetInum = startInum;
    childName = "";

    if (path.empty() || path[0] != '/') return false;
    if (path == "/") return true;

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
    if (parts.empty()) return false;

    childName = parts.back();

    for (size_t i = 0; i < parts.size(); i++) {
        uint64_t found = lookup_in_dir(parentInum, parts[i]);
        if (i == parts.size() - 1) {
            targetInum = found;
            return true;
        } else {
            if (found == 0) return false;
            RXFS_INODE inode{};
            if (!read_inode(found, inode)) return false;
            if (inode.file_type != RXFS_TYPE_DIRECTORY) return false;
            parentInum = found;
        }
    }
    return true;
}

static bool resolve_path(const std::string& path, uint64_t& parentInum, std::string& childName, uint64_t& targetInum) {
    if (path.empty() || path[0] != '/') return false;

    if (path == "/.snapshots") {
        parentInum = 1; targetInum = 999999; childName = ".snapshots";
        return true;
    }

    if (path.rfind("/.snapshots/", 0) == 0) {
        std::string snapSubPath = path.substr(12);
        size_t slashPos = snapSubPath.find('/');
        std::string snapName = (slashPos == std::string::npos) ? snapSubPath : snapSubPath.substr(0, slashPos);

        uint64_t snapInum = find_snapshot_root_by_name(snapName);
        if (snapInum == 0) return false;

        if (slashPos == std::string::npos) {
            parentInum = 999999; targetInum = snapInum; childName = snapName;
            return true;
        }

        std::string remPath = snapSubPath.substr(slashPos);
        return resolve_path_from(remPath, snapInum, parentInum, childName, targetInum);
    }

    return resolve_path_from(path, 1, parentInum, childName, targetInum);
}

static bool add_dir_entry(uint64_t parentInum, uint64_t childInum, uint8_t fileType, const std::string& name) {
    RXFS_INODE parent{};
    if (!read_inode(parentInum, parent)) return false;

    uint16_t reqLen = sizeof(RXFS_DIR_ENTRY) + name.size() + 1;
    reqLen = (reqLen + 7) & ~7;

    uint64_t blockIdx = 0;
    while (true) {
        uint64_t lba = get_file_block_lba(parent, blockIdx);
        if (lba == 0) {
            lba = allocate_file_block(parent, blockIdx);
            if (lba == 0) return false;
            if (blockIdx > 0) parent.flags |= INODE_FLAG_BTREE_DIRS;
        }

        std::vector<uint8_t> dirBuf(RXFS_BLOCK_SIZE);
        read_block(lba, dirBuf.data());

        size_t offset = 0;
        while (offset < dirBuf.size()) {
            const RXFS_DIR_ENTRY* entry = reinterpret_cast<const RXFS_DIR_ENTRY*>(&dirBuf[offset]);
            if (entry->entry_length == 0 || entry->inode_number == 0) break;
            offset += entry->entry_length;
        }

        if (offset + reqLen <= dirBuf.size()) {
            RXFS_DIR_ENTRY* newEntry = reinterpret_cast<RXFS_DIR_ENTRY*>(&dirBuf[offset]);
            newEntry->inode_number = childInum;
            newEntry->file_type = fileType;
            newEntry->name_length = name.size();
            newEntry->entry_length = reqLen;
            std::memcpy(newEntry->name, name.c_str(), name.size() + 1);

            log_journal_tx(JOP_WRITE_BLOCK, lba, dirBuf.data());
            write_block(lba, dirBuf.data());

            if ((blockIdx * RXFS_BLOCK_SIZE) + offset + reqLen > parent.file_size) {
                parent.file_size = (blockIdx * RXFS_BLOCK_SIZE) + offset + reqLen;
            }
            parent.mtime_us = now_us();
            write_inode(parentInum, parent);
            return true;
        }

        blockIdx++;
    }
    return false;
}

// ---------------- FUSE Callbacks (Phase M Master Final) ----------------

static void* rxfs_fuse_init(struct fuse_conn_info* conn, struct fuse_config* cfg) {
    (void)conn;
    cfg->kernel_cache = 1;
    return NULL;
}

static int rxfs_fuse_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void)fi;
    std::memset(stbuf, 0, sizeof(struct stat));

    if (std::strcmp(path, "/.snapshots") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_size = 4096;
        stbuf->st_ino = 999999;
        return 0;
    }

    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;

    if (inode.file_type == RXFS_TYPE_DIRECTORY)
        stbuf->st_mode = S_IFDIR | inode.permissions;
    else if (inode.file_type == RXFS_TYPE_SYMLINK)
        stbuf->st_mode = S_IFLNK | inode.permissions;
    else
        stbuf->st_mode = S_IFREG | inode.permissions;

    stbuf->st_nlink = inode.link_count;
    stbuf->st_size = inode.file_size;
    stbuf->st_ino = inode.inode_number;
    stbuf->st_blocks = inode.allocated_blocks * (RXFS_BLOCK_SIZE / 512);
    return 0;
}

static int rxfs_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                              off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;

    if (std::strcmp(path, "/.snapshots") == 0) {
        filler(buf, ".", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buf, "..", NULL, 0, static_cast<fuse_fill_dir_flags>(0));

        for (uint64_t i = 1; i <= g_state.sb.total_inodes; i++) {
            RXFS_INODE inode{};
            if (read_inode(i, inode) && (inode.flags & INODE_FLAG_SNAPSHOT) && inode.tags[0] != '\0') {
                filler(buf, reinterpret_cast<const char*>(inode.tags), NULL, 0, static_cast<fuse_fill_dir_flags>(0));
            }
        }
        return 0;
    }

    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE dirInode{};
    if (!read_inode(targetInum, dirInode)) return -ENOENT;
    if (dirInode.file_type != RXFS_TYPE_DIRECTORY) return -ENOTDIR;

    uint64_t totalBlocks = (dirInode.file_size + RXFS_BLOCK_SIZE - 1) / RXFS_BLOCK_SIZE;
    if (totalBlocks == 0) totalBlocks = 1;

    for (uint64_t bIdx = 0; bIdx < totalBlocks; bIdx++) {
        uint64_t lba = get_file_block_lba(dirInode, bIdx);
        if (lba == 0) continue;

        std::vector<uint8_t> dirBuf(RXFS_BLOCK_SIZE);
        if (!read_block(lba, dirBuf.data())) continue;

        size_t off = 0;
        while (off < dirBuf.size()) {
            const RXFS_DIR_ENTRY* entry = reinterpret_cast<const RXFS_DIR_ENTRY*>(&dirBuf[off]);
            if (entry->entry_length == 0) break;
            if (entry->inode_number != 0) {
                filler(buf, entry->name, NULL, 0, static_cast<fuse_fill_dir_flags>(0));
            }
            off += entry->entry_length;
        }
    }

    if (std::strcmp(path, "/") == 0) {
        filler(buf, ".snapshots", NULL, 0, static_cast<fuse_fill_dir_flags>(0));
    }

    return 0;
}

// O_TRUNC Support Included
static int rxfs_fuse_open(const char* path, struct fuse_file_info* fi) {
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;
    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    if (fi->flags & O_TRUNC) {
        RXFS_INODE inode{};
        if (read_inode(targetInum, inode)) {
            if (inode.flags & INODE_FLAG_IMMUTABLE) return -EPERM;
            inode.file_size = 0;
            inode.mtime_us = now_us();
            write_inode(targetInum, inode);
        }
    }
    return 0;
}

static int rxfs_fuse_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)fi;

    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;
    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;

    if (offset >= (off_t)inode.file_size) return 0;
    if (offset + size > inode.file_size) size = inode.file_size - offset;

    size_t total_read = 0;
    off_t cur_offset = offset;
    size_t remaining = size;

    while (remaining > 0) {
        uint64_t block_idx = cur_offset / RXFS_BLOCK_SIZE;
        uint64_t block_off = cur_offset % RXFS_BLOCK_SIZE;
        size_t to_read = std::min(remaining, (size_t)(RXFS_BLOCK_SIZE - block_off));

        uint64_t physical_lba = get_file_block_lba(inode, block_idx);
        if (physical_lba == 0) {
            std::memset(buf + total_read, 0, to_read);
        } else {
            std::vector<uint8_t> diskBlock(RXFS_BLOCK_SIZE);
            if (!read_block(physical_lba, diskBlock.data())) return -EIO;

            std::vector<uint8_t> plainBlock(RXFS_BLOCK_SIZE);

            if (inode.flags & INODE_FLAG_ENCRYPTED) {
                if (!aes_256_xts_crypt(diskBlock.data(), plainBlock.data(), RXFS_BLOCK_SIZE, physical_lba, false)) {
                    return -EIO;
                }
            } else {
                std::memcpy(plainBlock.data(), diskBlock.data(), RXFS_BLOCK_SIZE);
            }

            const ZstdBlockHeader* zhdr = reinterpret_cast<const ZstdBlockHeader*>(plainBlock.data());
            std::vector<uint8_t> decompBuf(RXFS_BLOCK_SIZE, 0);

            if (zhdr->magic == ZSTD_BLOCK_MAGIC) {
                uint8_t currentHash[32];
                compute_sha256(plainBlock.data() + sizeof(ZstdBlockHeader), zhdr->comp_size, currentHash);

                if (std::memcmp(currentHash, zhdr->block_sha256, 32) != 0) {
                    std::println(stderr, "[-] [BIT-ROT CORRUPTION DETECTED!] Block LBA {} Hash Mismatch!", physical_lba);
                    return -EIO;
                }

                size_t decompSize = ZSTD_decompress(decompBuf.data(), RXFS_BLOCK_SIZE,
                                                     plainBlock.data() + sizeof(ZstdBlockHeader), zhdr->comp_size);
                if (ZSTD_isError(decompSize)) return -EIO;
            } else {
                std::memcpy(decompBuf.data(), plainBlock.data(), RXFS_BLOCK_SIZE);
            }

            std::memcpy(buf + total_read, decompBuf.data() + block_off, to_read);
        }

        total_read += to_read;
        cur_offset += to_read;
        remaining -= to_read;
    }

    return total_read;
}

static int rxfs_fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    (void)fi;
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum != 0) return -EEXIST;

    uint64_t inum = allocate_inode();
    if (inum == 0) return -ENOSPC;

    RXFS_INODE inode{};
    inode.inode_number = inum;
    inode.file_type = RXFS_TYPE_REGULAR;
    inode.permissions = mode & 0777;
    inode.file_size = 0;
    inode.allocated_blocks = 0;
    inode.link_count = 1;
    inode.btime_us = now_us();
    inode.mtime_us = inode.btime_us;
    inode.compression_type = 2;
    inode.flags |= INODE_FLAG_ENCRYPTED;

    write_inode(inum, inode);
    add_dir_entry(parentInum, inum, RXFS_TYPE_REGULAR, childName);

    return 0;
}

static int rxfs_fuse_mknod(const char* path, mode_t mode, dev_t rdev) {
    (void)rdev;
    return rxfs_fuse_create(path, mode, NULL);
}

static int rxfs_fuse_link(const char* from, const char* to) {
    uint64_t parentInumFrom = 0, targetInumFrom = 0;
    std::string childNameFrom;
    if (!resolve_path(from, parentInumFrom, childNameFrom, targetInumFrom)) return -ENOENT;
    if (targetInumFrom == 0) return -ENOENT;

    uint64_t parentInumTo = 0, targetInumTo = 0;
    std::string childNameTo;
    if (!resolve_path(to, parentInumTo, childNameTo, targetInumTo)) return -ENOENT;
    if (targetInumTo != 0) return -EEXIST;

    RXFS_INODE inode{};
    if (!read_inode(targetInumFrom, inode)) return -ENOENT;

    inode.link_count++;
    write_inode(targetInumFrom, inode);

    add_dir_entry(parentInumTo, targetInumFrom, inode.file_type, childNameTo);
    return 0;
}

static int rxfs_fuse_symlink(const char* to, const char* from) {
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(from, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum != 0) return -EEXIST;

    uint64_t inum = allocate_inode();
    uint64_t blockLba = allocate_data_block();
    if (inum == 0 || blockLba == 0) return -ENOSPC;

    size_t toLen = std::strlen(to);
    std::vector<uint8_t> blockBuf(RXFS_BLOCK_SIZE, 0);
    std::memcpy(blockBuf.data(), to, std::min(toLen, (size_t)RXFS_BLOCK_SIZE));
    write_block(blockLba, blockBuf.data());

    RXFS_INODE inode{};
    inode.inode_number = inum;
    inode.file_type = RXFS_TYPE_SYMLINK;
    inode.permissions = 0777;
    inode.file_size = toLen;
    inode.allocated_blocks = 1;
    inode.link_count = 1;
    inode.direct_blocks[0] = blockLba;
    inode.btime_us = now_us();
    inode.mtime_us = inode.btime_us;

    write_inode(inum, inode);
    add_dir_entry(parentInum, inum, RXFS_TYPE_SYMLINK, childName);
    return 0;
}

static int rxfs_fuse_readlink(const char* path, char* buf, size_t size) {
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;
    if (inode.file_type != RXFS_TYPE_SYMLINK) return -EINVAL;

    std::vector<uint8_t> blockBuf(RXFS_BLOCK_SIZE);
    if (!read_block(inode.direct_blocks[0], blockBuf.data())) return -EIO;

    size_t copyLen = std::min(size - 1, (size_t)inode.file_size);
    std::memcpy(buf, blockBuf.data(), copyLen);
    buf[copyLen] = '\0';
    return 0;
}

static int rxfs_fuse_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)fi;
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;

    if (inode.flags & INODE_FLAG_IMMUTABLE) return -EPERM;

    if ((inode.flags & INODE_FLAG_APPEND_ONLY) && (uint64_t)offset < inode.file_size) {
        return -EPERM;
    }

    size_t total_written = 0;
    off_t cur_offset = offset;
    size_t remaining = size;

    while (remaining > 0) {
        uint64_t block_idx = cur_offset / RXFS_BLOCK_SIZE;
        uint64_t block_off = cur_offset % RXFS_BLOCK_SIZE;
        size_t to_write = std::min(remaining, (size_t)(RXFS_BLOCK_SIZE - block_off));

        uint64_t existingLba = get_file_block_lba(inode, block_idx);
        std::vector<uint8_t> uncompBlock(RXFS_BLOCK_SIZE, 0);

        if (existingLba != 0) {
            std::vector<uint8_t> diskBlock(RXFS_BLOCK_SIZE);
            read_block(existingLba, diskBlock.data());

            if (inode.flags & INODE_FLAG_ENCRYPTED) {
                aes_256_xts_crypt(diskBlock.data(), uncompBlock.data(), RXFS_BLOCK_SIZE, existingLba, false);
            } else {
                std::memcpy(uncompBlock.data(), diskBlock.data(), RXFS_BLOCK_SIZE);
            }

            const ZstdBlockHeader* zhdr = reinterpret_cast<const ZstdBlockHeader*>(uncompBlock.data());
            if (zhdr->magic == ZSTD_BLOCK_MAGIC) {
                std::vector<uint8_t> decompBuf(RXFS_BLOCK_SIZE, 0);
                ZSTD_decompress(decompBuf.data(), RXFS_BLOCK_SIZE, uncompBlock.data() + sizeof(ZstdBlockHeader), zhdr->comp_size);
                std::memcpy(uncompBlock.data(), decompBuf.data(), RXFS_BLOCK_SIZE);
            }
        }

        std::memcpy(uncompBlock.data() + block_off, buf + total_written, to_write);

        uint64_t physical_lba = allocate_data_block();
        if (physical_lba == 0) {
            if (total_written == 0) return -ENOSPC;
            break;
        }

        std::vector<uint8_t> compBlock(RXFS_BLOCK_SIZE, 0);
        size_t compBound = ZSTD_compressBound(RXFS_BLOCK_SIZE);
        std::vector<uint8_t> tempComp(compBound);

        size_t compSize = ZSTD_compress(tempComp.data(), compBound, uncompBlock.data(), RXFS_BLOCK_SIZE, 3);

        if (!ZSTD_isError(compSize) && (compSize + sizeof(ZstdBlockHeader) < RXFS_BLOCK_SIZE)) {
            ZstdBlockHeader zhdr{};
            zhdr.magic = ZSTD_BLOCK_MAGIC;
            zhdr.comp_size = (uint32_t)compSize;
            zhdr.orig_size = RXFS_BLOCK_SIZE;
            zhdr.crc32 = crc32c(uncompBlock.data(), RXFS_BLOCK_SIZE);
            compute_sha256(tempComp.data(), compSize, zhdr.block_sha256);

            std::memcpy(compBlock.data(), &zhdr, sizeof(zhdr));
            std::memcpy(compBlock.data() + sizeof(zhdr), tempComp.data(), compSize);
            inode.flags |= INODE_FLAG_COMPRESSED;
        } else {
            std::memcpy(compBlock.data(), uncompBlock.data(), RXFS_BLOCK_SIZE);
        }

        std::vector<uint8_t> cipherBlock(RXFS_BLOCK_SIZE, 0);
        if (!aes_256_xts_crypt(compBlock.data(), cipherBlock.data(), RXFS_BLOCK_SIZE, physical_lba, true)) {
            return -EIO;
        }
        inode.flags |= INODE_FLAG_ENCRYPTED;

        log_journal_tx(JOP_WRITE_BLOCK, physical_lba, cipherBlock.data());
        write_block(physical_lba, cipherBlock.data());

        if (block_idx < DIRECT_BLOCKS) {
            if (inode.direct_blocks[block_idx] == 0) inode.allocated_blocks++;
            inode.direct_blocks[block_idx] = physical_lba;
        }

        total_written += to_write;
        cur_offset += to_write;
        remaining -= to_write;
    }

    if ((uint64_t)cur_offset > inode.file_size) inode.file_size = cur_offset;
    inode.mtime_us = now_us();
    write_inode(targetInum, inode);

    return total_written;
}

static int rxfs_fuse_truncate(const char* path, off_t size, struct fuse_file_info* fi) {
    (void)fi;
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;
    if (inode.flags & INODE_FLAG_IMMUTABLE) return -EPERM;
    if (inode.flags & INODE_FLAG_APPEND_ONLY) return -EPERM;

    inode.file_size = size;
    inode.mtime_us = now_us();
    write_inode(targetInum, inode);
    return 0;
}

static int rxfs_fuse_mkdir(const char* path, mode_t mode) {
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum != 0) return -EEXIST;

    uint64_t inum = allocate_inode();
    uint64_t blockLba = allocate_data_block();
    if (inum == 0 || blockLba == 0) return -ENOSPC;

    std::vector<uint8_t> dirBlock(RXFS_BLOCK_SIZE, 0);
    size_t off = 0;

    auto add_entry = [&](uint64_t targetIn, const char* entryName) {
        RXFS_DIR_ENTRY* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(&dirBlock[off]);
        entry->inode_number = targetIn;
        entry->file_type = RXFS_TYPE_DIRECTORY;
        entry->name_length = std::strlen(entryName);
        uint16_t elen = (sizeof(RXFS_DIR_ENTRY) + entry->name_length + 1 + 7) & ~7;
        entry->entry_length = elen;
        std::memcpy(entry->name, entryName, entry->name_length + 1);
        off += elen;
    };

    add_entry(inum, ".");
    add_entry(parentInum, "..");
    write_block(blockLba, dirBlock.data());

    RXFS_INODE inode{};
    inode.inode_number = inum;
    inode.file_type = RXFS_TYPE_DIRECTORY;
    inode.permissions = mode & 0777;
    inode.file_size = off;
    inode.allocated_blocks = 1;
    inode.link_count = 2;
    inode.direct_blocks[0] = blockLba;
    inode.btime_us = now_us();
    inode.mtime_us = inode.btime_us;

    write_inode(inum, inode);
    add_dir_entry(parentInum, inum, RXFS_TYPE_DIRECTORY, childName);
    return 0;
}

static int rxfs_fuse_unlink(const char* path) {
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;

    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;

    RXFS_INODE inode{};
    if (!read_inode(targetInum, inode)) return -ENOENT;
    if (inode.flags & INODE_FLAG_IMMUTABLE) return -EPERM;
    if (inode.flags & INODE_FLAG_APPEND_ONLY) return -EPERM;

    if (inode.link_count > 1) {
        inode.link_count--;
        write_inode(targetInum, inode);
    } else {
        free_all_file_blocks(inode);
        free_inode_num(targetInum);
    }

    RXFS_INODE parent{};
    read_inode(parentInum, parent);
    uint64_t totalBlocks = (parent.file_size + RXFS_BLOCK_SIZE - 1) / RXFS_BLOCK_SIZE;

    for (uint64_t bIdx = 0; bIdx < totalBlocks; bIdx++) {
        uint64_t lba = get_file_block_lba(parent, bIdx);
        if (lba == 0) continue;

        std::vector<uint8_t> dirBuf(RXFS_BLOCK_SIZE);
        read_block(lba, dirBuf.data());

        size_t off = 0;
        bool found = false;
        while (off < dirBuf.size()) {
            RXFS_DIR_ENTRY* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(&dirBuf[off]);
            if (entry->entry_length == 0) break;
            if (entry->inode_number != 0 && std::strcmp(entry->name, childName.c_str()) == 0) {
                entry->inode_number = 0;
                write_block(lba, dirBuf.data());
                found = true;
                break;
            }
            off += entry->entry_length;
        }
        if (found) break;
    }
    return 0;
}

static int rxfs_fuse_rmdir(const char* path) {
    return rxfs_fuse_unlink(path);
}

static int rxfs_fuse_utimens(const char* path, const struct timespec tv[2], struct fuse_file_info* fi) {
    (void)tv; (void)fi;
    uint64_t parentInum = 0, targetInum = 0;
    std::string childName;
    if (!resolve_path(path, parentInum, childName, targetInum)) return -ENOENT;
    if (targetInum == 0) return -ENOENT;
    return 0;
}

static int rxfs_fuse_statfs(const char* path, struct statvfs* stbuf) {
    (void)path;
    stbuf->f_bsize   = RXFS_BLOCK_SIZE;
    stbuf->f_frsize  = RXFS_BLOCK_SIZE;
    stbuf->f_blocks  = g_state.sb.total_blocks;
    stbuf->f_bfree   = g_state.sb.free_blocks;
    stbuf->f_bavail  = g_state.sb.free_blocks;
    stbuf->f_files   = g_state.sb.total_inodes;
    stbuf->f_ffree   = g_state.sb.free_inodes;
    stbuf->f_namemax = RXFS_MAX_FILENAME_LEN;
    return 0;
}

static const struct fuse_operations rxfs_oper = {
    .getattr  = rxfs_fuse_getattr,
    .readlink = rxfs_fuse_readlink,
    .mknod    = rxfs_fuse_mknod,
    .mkdir    = rxfs_fuse_mkdir,
    .unlink   = rxfs_fuse_unlink,
    .rmdir    = rxfs_fuse_rmdir,
    .symlink  = rxfs_fuse_symlink,
    .link     = rxfs_fuse_link,
    .truncate = rxfs_fuse_truncate,
    .open     = rxfs_fuse_open,
    .read     = rxfs_fuse_read,
    .write    = rxfs_fuse_write,
    .statfs   = rxfs_fuse_statfs,
    .readdir  = rxfs_fuse_readdir,
    .init     = rxfs_fuse_init,
    .create   = rxfs_fuse_create,
    .utimens  = rxfs_fuse_utimens,
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "Usage: mount.rxfs <rxfs_image> <mount_point>");
        return 1;
    }

    g_state.imagePath = argv[1];
    const char* mountPoint = argv[2];

    g_state.devFile = std::fopen(g_state.imagePath.c_str(), "r+b");
    if (!g_state.devFile) {
        std::println(stderr, "[-] Cannot open RXFS image for Read-Write: {}", g_state.imagePath);
        return 1;
    }

    std::fseek(g_state.devFile, RXFS_BLOCK_SIZE, SEEK_SET);
    if (std::fread(&g_state.sb, sizeof(g_state.sb), 1, g_state.devFile) != 1 ||
        std::strncmp(g_state.sb.magic, "RXFS1QB!", 8) != 0) {
        std::println(stderr, "[-] Invalid RXFS volume header!");
        return 1;
    }

    replay_journal_recovery();
    load_dedup_table();

    std::println("[+] RXFS Master Production Driver Enabled (O_TRUNC & Truncation Support)");
    std::println("[+] Volume: {} -> {}", g_state.imagePath, mountPoint);

    char* fuseArgs[] = {
        argv[0],
        const_cast<char*>(mountPoint),
        const_cast<char*>("-f")
    };

    return fuse_main(3, fuseArgs, &rxfs_oper, NULL);
}
