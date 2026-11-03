// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "../../rxfs/include/rxfs_format.hpp"

namespace starlix::kernel::rxfs {

using namespace starlix::rxfs;

constexpr uint64_t kNativeRxfsBlockSize = RXFS_BLOCK_SIZE;
constexpr uint32_t kNativeRxfsMaxPathBytes = 4096u;
constexpr uint32_t kNativeRxfsMaxComponentBytes = RXFS_MAX_FILENAME_LEN;
constexpr uint32_t kNativeRxfsDirectBlockCount = 12u;
constexpr uint32_t kNativeRxfsIndirectEntryCount =
    static_cast<uint32_t>(RXFS_BLOCK_SIZE / sizeof(uint64_t));
constexpr uint32_t kNativeRxfsMaxDataBlocks =
    kNativeRxfsDirectBlockCount + kNativeRxfsIndirectEntryCount;

using NativeRxfsReadBlock = bool (*)(void* context, uint64_t block_lba,
                                     void* output, uint32_t output_capacity);

struct NativeRxfsContext {
    void* io_context;
    NativeRxfsReadBlock read_block;
    uint64_t volume_blocks;
    RXFS_SUPERBLOCK superblock;
    bool valid;
};

struct NativeRxfsNode {
    uint64_t inode_number;
    uint64_t file_size;
    uint64_t allocated_blocks;
    uint32_t flags;
    uint16_t file_type;
    uint16_t permissions;
};

inline void rxfs_copy_bytes(void* destination, const void* source, size_t count) {
    auto* d = static_cast<uint8_t*>(destination);
    const auto* s = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
}

inline bool rxfs_bytes_equal(const uint8_t* a, const char* b, size_t count) {
    if (!a || !b) return false;
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != static_cast<uint8_t>(b[i])) return false;
    }
    return true;
}

inline uint32_t rxfs_crc32c(const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

inline bool rxfs_power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

inline bool rxfs_superblock_valid(RXFS_SUPERBLOCK superblock,
                                  uint64_t volume_blocks) {
    if (!rxfs_bytes_equal(reinterpret_cast<const uint8_t*>(superblock.magic),
                          RXFS_MAGIC, sizeof(RXFS_MAGIC) - 1u) ||
        superblock.version_major != RXFS_VERSION_MAJOR ||
        superblock.version_minor > RXFS_VERSION_MINOR ||
        superblock.block_size != RXFS_BLOCK_SIZE ||
        superblock.total_blocks < 8u ||
        superblock.total_blocks > volume_blocks ||
        superblock.total_inodes == 0u ||
        superblock.total_inodes > 0xFFFFFFFFFFFFFFFFULL / RXFS_INODE_SIZE ||
        superblock.features_required != 0u || superblock.features_ro_compat != 0u ||
        superblock.root_inode == 0u ||
        superblock.root_inode > superblock.total_inodes ||
        superblock.max_filename_len == 0u ||
        superblock.max_filename_len > RXFS_MAX_FILENAME_LEN) return false;

    const uint64_t inode_bytes = superblock.total_inodes * RXFS_INODE_SIZE;
    if (superblock.total_inodes != 0u &&
        inode_bytes / RXFS_INODE_SIZE != superblock.total_inodes) return false;
    if (inode_bytes > 0xFFFFFFFFFFFFFFFFULL - (RXFS_BLOCK_SIZE - 1u)) return false;
    const uint64_t inode_table_blocks =
        (inode_bytes + RXFS_BLOCK_SIZE - 1u) / RXFS_BLOCK_SIZE;
    if (inode_table_blocks == 0u ||
        superblock.inode_table_lba >= superblock.total_blocks ||
        inode_table_blocks > superblock.total_blocks - superblock.inode_table_lba ||
        superblock.data_blocks_lba < superblock.inode_table_lba + inode_table_blocks ||
        superblock.data_blocks_lba >= superblock.total_blocks ||
        superblock.inode_bitmap_lba == 0u ||
        superblock.block_bitmap_lba == 0u ||
        superblock.inode_bitmap_lba >= superblock.data_blocks_lba ||
        superblock.block_bitmap_lba >= superblock.data_blocks_lba) return false;

    if (superblock.journal_size_blocks != 0u) {
        if (superblock.journal_lba == 0u ||
            superblock.journal_lba >= superblock.total_blocks ||
            superblock.journal_size_blocks >
                superblock.total_blocks - superblock.journal_lba) return false;
    }

    const uint32_t stored_checksum = superblock.superblock_checksum;
    superblock.superblock_checksum = 0u;
    if (rxfs_crc32c(&superblock, sizeof(superblock) - sizeof(uint32_t)) !=
        stored_checksum) return false;

    return true;
}

inline bool rxfs_read_block(const NativeRxfsContext* context, uint64_t block_lba,
                            void* output) {
    if (!context || !context->valid || !context->read_block || !output ||
        block_lba >= context->superblock.total_blocks) return false;
    return context->read_block(context->io_context, block_lba, output,
                               static_cast<uint32_t>(RXFS_BLOCK_SIZE));
}

inline bool MountNativeRxfs(void* io_context, NativeRxfsReadBlock read_block,
                            uint64_t volume_blocks, NativeRxfsContext* context) {
    if (!context) return false;
    *context = {};
    if (!io_context || !read_block || volume_blocks < 2u) return false;

    constexpr uint64_t sectors_per_block = RXFS_BLOCK_SIZE / RXFS_SECTOR_SIZE;
    if (volume_blocks > 0xFFFFFFFFFFFFFFFFULL / sectors_per_block) return false;
    const uint64_t volume_sectors = volume_blocks * sectors_per_block;

    alignas(16) uint8_t block[RXFS_BLOCK_SIZE]{};
    if (!read_block(io_context, 0u, block, sizeof(block))) return false;

    RXFS_BOOT_SECTOR boot{};
    rxfs_copy_bytes(&boot, block, sizeof(boot));
    if (!rxfs_bytes_equal(reinterpret_cast<const uint8_t*>(boot.oem_name),
                          RXFS_MAGIC, 8u) ||
        boot.sector_size != RXFS_SECTOR_SIZE ||
        !rxfs_power_of_two(boot.sectors_per_cluster) ||
        boot.sectors_per_cluster > 128u ||
        boot.reserved_sectors == 0u ||
        boot.total_sectors < sectors_per_block * 2u ||
        boot.total_sectors > volume_sectors ||
        boot.backup_sb_lba == 0u ||
        boot.backup_sb_lba >= boot.total_sectors ||
        boot.superblock_lba != 1u || boot.boot_signature != 0xAA55u) return false;

    if (!read_block(io_context, 1u, block, sizeof(block))) return false;
    RXFS_SUPERBLOCK superblock{};
    rxfs_copy_bytes(&superblock, block, sizeof(superblock));
    if (!rxfs_superblock_valid(superblock, volume_blocks) ||
        superblock.total_blocks > boot.total_sectors / sectors_per_block) return false;
    for (size_t i = 0u; i < sizeof(boot.volume_uuid); ++i) {
        if (boot.volume_uuid[i] != superblock.volume_uuid[i]) return false;
    }

    context->io_context = io_context;
    context->read_block = read_block;
    context->volume_blocks = volume_blocks;
    context->superblock = superblock;
    context->valid = true;
    return true;
}

inline bool rxfs_read_inode(const NativeRxfsContext* context, uint64_t inode_number,
                            RXFS_INODE* output) {
    if (!context || !context->valid || !output || inode_number == 0u ||
        inode_number > context->superblock.total_inodes) return false;

    const uint64_t inode_index = inode_number - 1u;
    const uint64_t byte_offset = inode_index * RXFS_INODE_SIZE;
    const uint64_t block_lba = context->superblock.inode_table_lba +
                               byte_offset / RXFS_BLOCK_SIZE;
    const uint32_t in_block = static_cast<uint32_t>(byte_offset % RXFS_BLOCK_SIZE);
    if (block_lba >= context->superblock.data_blocks_lba ||
        in_block + sizeof(RXFS_INODE) > RXFS_BLOCK_SIZE) return false;

    alignas(16) uint8_t block[RXFS_BLOCK_SIZE]{};
    if (!rxfs_read_block(context, block_lba, block)) return false;
    RXFS_INODE inode{};
    rxfs_copy_bytes(&inode, block + in_block, sizeof(inode));
    if (inode.inode_number != inode_number ||
        (inode.file_type != RXFS_TYPE_REGULAR &&
         inode.file_type != RXFS_TYPE_DIRECTORY)) return false;

    const uint32_t stored_checksum = inode.inode_checksum;
    inode.inode_checksum = 0u;
    if (rxfs_crc32c(&inode, sizeof(inode) - sizeof(uint32_t)) != stored_checksum)
        return false;
    inode.inode_checksum = stored_checksum;
    *output = inode;
    return true;
}

inline bool rxfs_component_equal(const char* left, size_t left_length,
                                 const char* right, size_t right_length) {
    if (!left || !right || left_length != right_length) return false;
    for (size_t i = 0; i < left_length; ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

inline bool rxfs_load_indirect_table(const NativeRxfsContext* context,
                                     const RXFS_INODE& inode,
                                     uint64_t* output_entries) {
    if (!context || !context->valid || !output_entries || inode.indirect_block == 0u ||
        inode.indirect_block < context->superblock.data_blocks_lba ||
        inode.indirect_block >= context->superblock.total_blocks) return false;
    alignas(16) uint8_t block[RXFS_BLOCK_SIZE]{};
    if (!rxfs_read_block(context, inode.indirect_block, block)) return false;
    rxfs_copy_bytes(output_entries, block, RXFS_BLOCK_SIZE);
    return true;
}

inline bool rxfs_get_data_block_lba(const NativeRxfsContext* context,
                                    const RXFS_INODE& inode,
                                    uint64_t logical_index,
                                    const uint64_t* indirect_entries,
                                    uint64_t* output_lba) {
    if (!context || !context->valid || !output_lba ||
        logical_index >= kNativeRxfsMaxDataBlocks) return false;
    uint64_t lba = 0u;
    if (logical_index < kNativeRxfsDirectBlockCount) {
        lba = inode.direct_blocks[logical_index];
    } else {
        const uint64_t indirect_index = logical_index - kNativeRxfsDirectBlockCount;
        if (!indirect_entries || inode.indirect_block == 0u ||
            indirect_index >= kNativeRxfsIndirectEntryCount) return false;
        lba = indirect_entries[indirect_index];
    }
    if (lba < context->superblock.data_blocks_lba ||
        lba >= context->superblock.total_blocks ||
        (inode.indirect_block != 0u && lba == inode.indirect_block)) return false;
    *output_lba = lba;
    return true;
}

inline bool rxfs_inode_data_layout_valid(const NativeRxfsContext* context,
                                         const RXFS_INODE& inode,
                                         uint64_t required_data_blocks) {
    if (!context || !context->valid ||
        required_data_blocks > kNativeRxfsMaxDataBlocks ||
        inode.allocated_blocks > kNativeRxfsMaxDataBlocks + 1u ||
        inode.double_indirect != 0u || inode.triple_indirect != 0u ||
        inode.extent_tree_root != 0u) return false;
    const uint64_t metadata_blocks = required_data_blocks >
        kNativeRxfsDirectBlockCount ? 1u : 0u;
    if (required_data_blocks + metadata_blocks > inode.allocated_blocks) return false;
    if (required_data_blocks > kNativeRxfsDirectBlockCount) {
        if (inode.indirect_block == 0u ||
            inode.indirect_block < context->superblock.data_blocks_lba ||
            inode.indirect_block >= context->superblock.total_blocks) return false;
    }
    return true;
}

inline bool rxfs_lookup_child(const NativeRxfsContext* context,
                              const RXFS_INODE& directory,
                              const char* component, size_t component_length,
                              uint64_t* child_inode) {
    if (!context || !context->valid || !component || component_length == 0u ||
        component_length > context->superblock.max_filename_len || !child_inode ||
        directory.file_type != RXFS_TYPE_DIRECTORY ||
        (directory.flags & (INODE_FLAG_BTREE_DIRS | INODE_FLAG_COMPRESSED |
                            INODE_FLAG_ENCRYPTED | INODE_FLAG_SPARSE |
                            INODE_FLAG_DEDUPLICATED | INODE_FLAG_REFLINK)) != 0u ||
        directory.file_size >
            static_cast<uint64_t>(kNativeRxfsMaxDataBlocks) * RXFS_BLOCK_SIZE) return false;

    const uint64_t required_data_blocks =
        (directory.file_size + RXFS_BLOCK_SIZE - 1u) / RXFS_BLOCK_SIZE;
    if (!rxfs_inode_data_layout_valid(context, directory, required_data_blocks))
        return false;
    alignas(16) uint64_t indirect_entries[kNativeRxfsIndirectEntryCount]{};
    const uint64_t* indirect = nullptr;
    if (required_data_blocks > kNativeRxfsDirectBlockCount) {
        if (!rxfs_load_indirect_table(context, directory, indirect_entries)) return false;
        indirect = indirect_entries;
    }

    uint64_t remaining = directory.file_size;
    alignas(16) uint8_t block[RXFS_BLOCK_SIZE]{};
    for (uint64_t index = 0u; index < required_data_blocks && remaining != 0u;
         ++index) {
        uint64_t lba = 0u;
        if (!rxfs_get_data_block_lba(context, directory, index, indirect, &lba) ||
            !rxfs_read_block(context, lba, block)) return false;
        const size_t used = remaining < RXFS_BLOCK_SIZE
            ? static_cast<size_t>(remaining) : static_cast<size_t>(RXFS_BLOCK_SIZE);
        size_t offset = 0u;
        while (offset < used) {
            if (used - offset < sizeof(RXFS_DIR_ENTRY)) return false;
            const auto* entry = reinterpret_cast<const RXFS_DIR_ENTRY*>(block + offset);
            const size_t minimum_length = sizeof(RXFS_DIR_ENTRY) +
                                          static_cast<size_t>(entry->name_length) + 1u;
            if (entry->entry_length == 0u) break;
            if (entry->entry_length < minimum_length ||
                (entry->entry_length & 7u) != 0u ||
                entry->entry_length > used - offset ||
                entry->name_length == 0u ||
                entry->name_length > context->superblock.max_filename_len ||
                entry->name[entry->name_length] != '\0') return false;
            for (size_t n = 0; n < entry->name_length; ++n) {
                if (entry->name[n] == '\0' || entry->name[n] == '/' ||
                    entry->name[n] == '\\') return false;
            }
            if (entry->inode_number != 0u &&
                rxfs_component_equal(component, component_length, entry->name,
                                     entry->name_length)) {
                if (entry->inode_number > context->superblock.total_inodes) return false;
                *child_inode = entry->inode_number;
                return true;
            }
            offset += entry->entry_length;
        }
        remaining -= used;
    }
    return false;
}

inline bool rxfs_is_prefix_name(const char* component, size_t length,
                                const char* expected) {
    size_t expected_length = 0u;
    while (expected[expected_length] != '\0') ++expected_length;
    if (length != expected_length) return false;
    for (size_t i = 0; i < length; ++i) {
        char a = component[i];
        char b = expected[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

inline bool rxfs_resolve_path(const NativeRxfsContext* context, const char* path,
                              NativeRxfsNode* output) {
    if (!context || !context->valid || !path || !output) return false;
    size_t path_length = 0u;
    while (path[path_length] != '\0' && path_length < kNativeRxfsMaxPathBytes)
        ++path_length;
    if (path_length == 0u || path_length >= kNativeRxfsMaxPathBytes) return false;

    RXFS_INODE current{};
    if (!rxfs_read_inode(context, context->superblock.root_inode, &current) ||
        current.file_type != RXFS_TYPE_DIRECTORY) return false;

    size_t cursor = 0u;
    while (cursor < path_length && (path[cursor] == '/' || path[cursor] == '\\'))
        ++cursor;
    bool first_component = true;
    while (cursor < path_length) {
        const size_t begin = cursor;
        while (cursor < path_length && path[cursor] != '/' && path[cursor] != '\\')
            ++cursor;
        const size_t length = cursor - begin;
        while (cursor < path_length && (path[cursor] == '/' || path[cursor] == '\\'))
            ++cursor;
        if (length == 0u) continue;
        if (length == 1u && path[begin] == '.') continue;
        if (length == 2u && path[begin] == '.' && path[begin + 1u] == '.') return false;
        if (first_component && rxfs_is_prefix_name(path + begin, length, "RXFS")) {
            first_component = false;
            continue;
        }
        first_component = false;
        if (length > context->superblock.max_filename_len) return false;
        uint64_t child = 0u;
        if (!rxfs_lookup_child(context, current, path + begin, length, &child) ||
            !rxfs_read_inode(context, child, &current)) return false;
    }

    output->inode_number = current.inode_number;
    output->file_size = current.file_size;
    output->allocated_blocks = current.allocated_blocks;
    output->flags = current.flags;
    output->file_type = current.file_type;
    output->permissions = current.permissions;
    return true;
}

inline bool LookupNativeRxfsPath(const NativeRxfsContext* context, const char* path,
                                 NativeRxfsNode* output) {
    return rxfs_resolve_path(context, path, output);
}

inline bool ReadNativeRxfsFile(const NativeRxfsContext* context, const char* path,
                               void* output, uint32_t output_capacity,
                               uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0u;
    if (!context || !context->valid || !path || !bytes_read) return false;
    NativeRxfsNode node{};
    if (!rxfs_resolve_path(context, path, &node) ||
        node.file_type != RXFS_TYPE_REGULAR ||
        node.file_size > output_capacity ||
        (node.file_size != 0u && !output)) return false;

    RXFS_INODE inode{};
    if (!rxfs_read_inode(context, node.inode_number, &inode) ||
        (inode.flags & (INODE_FLAG_COMPRESSED | INODE_FLAG_ENCRYPTED |
                        INODE_FLAG_SPARSE | INODE_FLAG_DEDUPLICATED |
                        INODE_FLAG_REFLINK | INODE_FLAG_BTREE_DIRS)) != 0u) return false;

    if (inode.file_size >
        static_cast<uint64_t>(kNativeRxfsMaxDataBlocks) * RXFS_BLOCK_SIZE) return false;
    const uint64_t required_blocks = (inode.file_size + RXFS_BLOCK_SIZE - 1u) /
                                     RXFS_BLOCK_SIZE;
    if (!rxfs_inode_data_layout_valid(context, inode, required_blocks)) return false;
    if (inode.file_size == 0u) return true;

    alignas(16) uint64_t indirect_entries[kNativeRxfsIndirectEntryCount]{};
    const uint64_t* indirect = nullptr;
    if (required_blocks > kNativeRxfsDirectBlockCount) {
        if (!rxfs_load_indirect_table(context, inode, indirect_entries)) return false;
        indirect = indirect_entries;
    }

    auto* destination = static_cast<uint8_t*>(output);
    uint64_t remaining = inode.file_size;
    alignas(16) uint8_t block[RXFS_BLOCK_SIZE]{};
    for (uint64_t index = 0u; index < required_blocks; ++index) {
        uint64_t lba = 0u;
        if (!rxfs_get_data_block_lba(context, inode, index, indirect, &lba) ||
            !rxfs_read_block(context, lba, block)) return false;
        const uint32_t amount = remaining < RXFS_BLOCK_SIZE
            ? static_cast<uint32_t>(remaining) : static_cast<uint32_t>(RXFS_BLOCK_SIZE);
        rxfs_copy_bytes(destination + *bytes_read, block, amount);
        *bytes_read += amount;
        remaining -= amount;
    }
    return remaining == 0u && *bytes_read == inode.file_size;
}

} // namespace starlix::kernel::rxfs
