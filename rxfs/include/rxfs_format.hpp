// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1
#include <cstdint>
#include <cstring>
#else
#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace std {
    inline void* memcpy(void* dest, const void* src, size_t n) {
        unsigned char* d = (unsigned char*)dest;
        const unsigned char* s = (const unsigned char*)src;
        for (size_t i = 0; i < n; i++) d[i] = s[i];
        return dest;
    }
    inline void* memset(void* dest, int val, size_t n) {
        unsigned char* d = (unsigned char*)dest;
        for (size_t i = 0; i < n; i++) d[i] = (unsigned char)val;
        return dest;
    }
    inline char* strncpy(char* dest, const char* src, size_t n) {
        size_t i = 0;
        for (; i < n && src[i] != '\0'; i++) dest[i] = src[i];
        for (; i < n; i++) dest[i] = '\0';
        return dest;
    }
    inline int strncmp(const char* s1, const char* s2, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (s1[i] != s2[i] || s1[i] == '\0' || s2[i] == '\0')
                return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
        return 0;
    }
}
#endif

namespace starlix::rxfs {

#pragma pack(push, 1)

// ============================================================
// RXFS Core Constants
// ============================================================
constexpr char     RXFS_MAGIC[]       = "RXFS1QB!";
constexpr uint32_t RXFS_VERSION_MAJOR = 1;
constexpr uint32_t RXFS_VERSION_MINOR = 0;
constexpr uint64_t RXFS_BLOCK_SIZE    = 4096;
constexpr uint64_t RXFS_SECTOR_SIZE   = 512;
constexpr uint64_t RXFS_INODE_SIZE    = 512;

constexpr uint64_t RXFS_MAX_VOLUME_SIZE  = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint64_t RXFS_MAX_FILE_SIZE    = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint32_t RXFS_MAX_FILENAME_LEN = 1024;
constexpr uint64_t RXFS_MAX_INODES       = 0xFFFFFFFFFFFFFFFFULL;

enum RxfsFeatures : uint64_t {
    FEAT_JOURNALING        = 1ULL << 0,
    FEAT_ACL               = 1ULL << 1,
    FEAT_SPARSE_FILES      = 1ULL << 2,
    FEAT_HARD_LINKS        = 1ULL << 3,
    FEAT_SOFT_LINKS        = 1ULL << 4,
    FEAT_ALT_STREAMS       = 1ULL << 5,
    FEAT_REPARSE_POINTS    = 1ULL << 6,
    FEAT_USN_JOURNAL       = 1ULL << 7,
    FEAT_QUOTAS            = 1ULL << 8,
    FEAT_COMPRESSION       = 1ULL << 9,
    FEAT_ENCRYPTION        = 1ULL << 10,
    FEAT_SHADOW_COPIES     = 1ULL << 11,
    FEAT_TRANSACTIONS      = 1ULL << 12,
    FEAT_BLAKE3_CHECKSUMS  = 1ULL << 13,
    FEAT_ZSTD_COMPRESSION  = 1ULL << 14,
    FEAT_AES256_XTS        = 1ULL << 15,
    FEAT_COW_SNAPSHOTS     = 1ULL << 16,
    FEAT_DEDUPLICATION     = 1ULL << 17,
    FEAT_UTF8_1024         = 1ULL << 18,
    FEAT_64BIT_INODES      = 1ULL << 19,
    FEAT_MICROSEC_TIME     = 1ULL << 20,
    FEAT_BIRTH_TIME        = 1ULL << 21,
    FEAT_FILE_RAID         = 1ULL << 22,
    FEAT_VERSIONING        = 1ULL << 23,
    FEAT_CAS_MODE          = 1ULL << 24,
    FEAT_ONLINE_DEFRAG     = 1ULL << 25,
    FEAT_TIERING           = 1ULL << 26,
    FEAT_PETA_SPARSE       = 1ULL << 27,
    FEAT_CASE_DUAL         = 1ULL << 28,
    FEAT_XATTR_UNLIMITED   = 1ULL << 29,
    FEAT_POSIX_SYMLINKS    = 1ULL << 30,
    FEAT_REFLINK           = 1ULL << 31,
    FEAT_AUTO_TRIM         = 1ULL << 32,
    FEAT_MULTI_STREAM      = 1ULL << 33,
    FEAT_PERDIR_QUOTAS     = 1ULL << 34,
    FEAT_ONLINE_FSCK       = 1ULL << 35,
    FEAT_ATOMIC_RENAME     = 1ULL << 36,
    FEAT_WRITE_BARRIERS    = 1ULL << 37,
    FEAT_CRC32C_ACCEL      = 1ULL << 38,
    FEAT_VARIABLE_BLOCK    = 1ULL << 39,
    FEAT_NS_ENCRYPTION     = 1ULL << 40,
    FEAT_PER_FILE_COMP     = 1ULL << 41,
    FEAT_IMMUTABLE_FILES   = 1ULL << 42,
    FEAT_APPEND_ONLY       = 1ULL << 43,
    FEAT_SECURE_DELETE     = 1ULL << 44,
    FEAT_ATIME_CONTROL     = 1ULL << 45,
    FEAT_CASE_FOLDING      = 1ULL << 46,
    FEAT_BTREE_DIRS        = 1ULL << 47,
    FEAT_MMAP_JOURNAL      = 1ULL << 48,
    FEAT_TRIM_ON_MOUNT     = 1ULL << 49,
    FEAT_AUTO_DEFRAG       = 1ULL << 50,
    FEAT_SCRUB             = 1ULL << 51,
    FEAT_ONLINE_RESIZE     = 1ULL << 52,
    FEAT_CROSS_DEV_MOVE    = 1ULL << 53,
    FEAT_UUID_LABEL_V2     = 1ULL << 54,
    FEAT_COMP_STATS        = 1ULL << 55,
    FEAT_IOPS_LIMITER      = 1ULL << 56,
    FEAT_SNAPSHOT_SEND     = 1ULL << 57,
    FEAT_KEY_PROMPT        = 1ULL << 58,
    FEAT_TPM_SEALED_KEY    = 1ULL << 59,
    FEAT_ROLLING_CKSUM     = 1ULL << 60,
    FEAT_FILE_TAGS         = 1ULL << 61,
    FEAT_CHANGE_NOTIFY     = 1ULL << 62,
};

constexpr uint64_t RXFS_FEATURES_ALL = 0xFFFFFFFFFFFFFFFFULL;

// ============================================================
// BOOT SECTOR (LBA 0) - Exactly 512 bytes
// ============================================================
struct RXFS_BOOT_SECTOR {
    uint8_t  jump_boot[3];
    char     oem_name[8];
    uint16_t sector_size;
    uint8_t  sectors_per_cluster;
    uint32_t reserved_sectors;
    uint64_t total_sectors;
    uint64_t superblock_lba;
    uint64_t backup_sb_lba;
    uint8_t  volume_uuid[16];
    char     volume_label[32];
    uint8_t  bootstrap_code[420];
    uint16_t boot_signature;
};
static_assert(sizeof(RXFS_BOOT_SECTOR) == 512, "Boot sector must be 512 bytes");

// ============================================================
// SUPERBLOCK (LBA 1-7) - Exactly 4096 bytes
// ============================================================
struct RXFS_SUPERBLOCK {
    char     magic[8];
    uint32_t version_major;
    uint32_t version_minor;
    uint8_t  volume_uuid[16];
    char     volume_label[64];

    uint64_t total_blocks;
    uint64_t block_size;
    uint64_t total_inodes;
    uint64_t free_blocks;
    uint64_t free_inodes;

    uint64_t max_volume_size;
    uint64_t max_file_size;
    uint32_t max_filename_len;

    uint64_t inode_bitmap_lba;
    uint64_t block_bitmap_lba;
    uint64_t journal_lba;
    uint64_t journal_size_blocks;
    uint64_t inode_table_lba;
    uint64_t data_blocks_lba;
    uint64_t root_inode;

    uint64_t features_enabled;
    uint64_t features_required;
    uint64_t features_ro_compat;

    uint64_t mount_count;
    uint64_t last_mount_time;
    uint64_t last_write_time;
    uint64_t last_fsck_time;
    uint64_t last_snapshot_time;

    uint64_t snapshot_root_lba;
    uint64_t dedup_table_lba;
    uint64_t xattr_tree_lba;

    uint32_t compression_algo;
    uint32_t encryption_algo;
    uint32_t checksum_algo;

    uint8_t  master_key_hash[32];
    uint32_t superblock_checksum;
    uint8_t  reserved[3748];
};
static_assert(sizeof(RXFS_SUPERBLOCK) == 4096, "Superblock must be 4096 bytes");

// ============================================================
// INODE - Exactly 512 bytes
// ============================================================
enum RxfsFileType : uint16_t {
    RXFS_TYPE_UNKNOWN   = 0,
    RXFS_TYPE_REGULAR   = 1,
    RXFS_TYPE_DIRECTORY = 2,
    RXFS_TYPE_SYMLINK   = 3,
    RXFS_TYPE_HARDLINK  = 4,
    RXFS_TYPE_DEVICE    = 5,
    RXFS_TYPE_FIFO      = 6,
    RXFS_TYPE_SOCKET    = 7,
    RXFS_TYPE_REPARSE   = 8,
};

enum RxfsInodeFlags : uint32_t {
    INODE_FLAG_COMPRESSED    = 1 << 0,
    INODE_FLAG_ENCRYPTED     = 1 << 1,
    INODE_FLAG_SPARSE        = 1 << 2,
    INODE_FLAG_IMMUTABLE     = 1 << 3,
    INODE_FLAG_APPEND_ONLY   = 1 << 4,
    INODE_FLAG_NO_ATIME      = 1 << 5,
    INODE_FLAG_SECURE_DELETE = 1 << 6,
    INODE_FLAG_DEDUPLICATED  = 1 << 7,
    INODE_FLAG_REFLINK       = 1 << 8,
    INODE_FLAG_SNAPSHOT      = 1 << 9,
    INODE_FLAG_VERSIONED     = 1 << 10,
    INODE_FLAG_HAS_XATTR     = 1 << 11,
    INODE_FLAG_HAS_ACL       = 1 << 12,
    INODE_FLAG_HAS_STREAMS   = 1 << 13,
    INODE_FLAG_BTREE_DIRS    = 1 << 14,
};

struct RXFS_INODE {
    uint64_t inode_number;
    uint16_t file_type;
    uint16_t permissions;
    uint32_t flags;
    uint64_t uid;
    uint64_t gid;
    uint64_t file_size;
    uint64_t allocated_blocks;
    uint64_t link_count;

    uint64_t atime_us;
    uint64_t mtime_us;
    uint64_t ctime_us;
    uint64_t btime_us;

    uint64_t direct_blocks[12];
    uint64_t indirect_block;
    uint64_t double_indirect;
    uint64_t triple_indirect;
    uint64_t extent_tree_root;

    uint64_t xattr_block;
    uint64_t acl_block;
    uint64_t stream_tree;
    uint64_t snapshot_prev;
    uint64_t dedup_hash;

    uint8_t  encryption_iv[16];
    uint32_t encryption_key_id;

    uint8_t  compression_type;
    uint8_t  reserved_c[3];
    uint64_t uncompressed_size;

    uint32_t inode_checksum;
    uint8_t  block_checksum_tree;

    uint8_t  tags[64];

    uint8_t  replica_count;
    uint8_t  reserved_r[7];

    uint8_t  reserved[147];
};
static_assert(sizeof(RXFS_INODE) == 512, "Inode must be exactly 512 bytes");

// ============================================================
// DIRECTORY ENTRY
// ============================================================
struct RXFS_DIR_ENTRY {
    uint64_t inode_number;
    uint16_t entry_length;
    uint16_t name_length;
    uint8_t  file_type;
    uint8_t  reserved[3];
    char     name[];
};

// ============================================================
// JOURNAL TRANSACTIONS (WAL Engine)
// ============================================================
enum RxfsJournalOp : uint32_t {
    JOP_BEGIN       = 1,
    JOP_WRITE_BLOCK = 2,
    JOP_UPDATE_INODE= 3,
    JOP_ALLOC_BLOCK = 4,
    JOP_FREE_BLOCK  = 5,
    JOP_RENAME      = 6,
    JOP_COMMIT      = 7,
    JOP_ABORT       = 8,
};

struct RXFS_JOURNAL_HEADER {
    uint32_t magic;
    uint32_t sequence;
    uint32_t op_type;
    uint32_t data_length;
    uint64_t timestamp_us;
    uint64_t target_lba;
    uint32_t checksum;
    uint32_t reserved;
};

#pragma pack(pop)

inline void init_superblock(RXFS_SUPERBLOCK& sb, uint64_t volume_bytes) {
    std::memset(&sb, 0, sizeof(sb));
    std::memcpy(sb.magic, RXFS_MAGIC, 8);
    sb.version_major = RXFS_VERSION_MAJOR;
    sb.version_minor = RXFS_VERSION_MINOR;
    sb.block_size = RXFS_BLOCK_SIZE;
    sb.total_blocks = volume_bytes / RXFS_BLOCK_SIZE;
    sb.max_volume_size = RXFS_MAX_VOLUME_SIZE;
    sb.max_file_size = RXFS_MAX_FILE_SIZE;
    sb.max_filename_len = RXFS_MAX_FILENAME_LEN;
    sb.features_enabled = RXFS_FEATURES_ALL;
    sb.root_inode = 1;
    sb.compression_algo = 2;
    sb.encryption_algo = 1;
    sb.checksum_algo = 1;
    std::strncpy(sb.volume_label, "RXFS_VOLUME", sizeof(sb.volume_label) - 1);
}

} // namespace starlix::rxfs
