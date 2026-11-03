/* Phase 20: bounded block/FAT32 write and GPT redundancy policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::storage_policy {
constexpr uint32_t kMaxTransferBytes = 4096u;
constexpr uint32_t kMaxInPlaceOverwriteBytes = 64u * 1024u;
constexpr bool block_range_allowed(bool present, uint32_t block_size,
                                   uint32_t last_lba, uint64_t lba,
                                   uint32_t block_count, uint64_t capacity) {
    if (!present || block_count == 0u || block_size < 512u ||
        block_size > 4096u || lba > last_lba) return false;
    const uint64_t bytes = static_cast<uint64_t>(block_size) * block_count;
    if (bytes > capacity || bytes > kMaxTransferBytes) return false;
    const uint64_t end = lba + block_count;
    if (end < lba || end > static_cast<uint64_t>(last_lba) + 1u) return false;
    return true;
}
constexpr bool overwrite_existing_file_allowed(bool mounted, bool found,
                                                bool directory,
                                                uint32_t old_size,
                                                uint32_t new_size,
                                                uint32_t bytes_per_sector,
                                                uint32_t cluster_count) {
    return mounted && found && !directory && old_size == new_size &&
           new_size <= kMaxInPlaceOverwriteBytes &&
           (bytes_per_sector == 512u || bytes_per_sector == 1024u ||
            bytes_per_sector == 2048u || bytes_per_sector == 4096u) &&
           cluster_count != 0u;
}
constexpr bool gpt_backup_header_relationship(uint64_t disk_last_lba,
                                             uint64_t primary_current,
                                             uint64_t primary_backup,
                                             uint64_t backup_current,
                                             uint64_t backup_backup,
                                             uint64_t primary_first_usable,
                                             uint64_t primary_last_usable,
                                             uint64_t backup_first_usable,
                                             uint64_t backup_last_usable,
                                             uint32_t primary_entry_count,
                                             uint32_t primary_entry_size,
                                             uint32_t backup_entry_count,
                                             uint32_t backup_entry_size,
                                             uint32_t primary_entries_crc,
                                             uint32_t backup_entries_crc,
                                             bool disk_guids_match) {
    return disk_guids_match && primary_current == 1u &&
           primary_backup == disk_last_lba && backup_current == primary_backup &&
           backup_backup == primary_current &&
           primary_first_usable == backup_first_usable &&
           primary_last_usable == backup_last_usable &&
           primary_entry_count == backup_entry_count &&
           primary_entry_size == backup_entry_size &&
           primary_entries_crc == backup_entries_crc;
}
constexpr bool self_test() {
    if (!block_range_allowed(true,512u,99u,10u,8u,4096u)) return false;
    if (block_range_allowed(true,512u,99u,10u,9u,4096u)) return false;
    if (block_range_allowed(true,512u,99u,99u,2u,4096u)) return false;
    if (block_range_allowed(false,512u,99u,0u,1u,512u)) return false;
    if (block_range_allowed(true,4096u,99u,0u,2u,8192u)) return false;
    if (!overwrite_existing_file_allowed(true,true,false,2048u,2048u,512u,8u)) return false;
    if (overwrite_existing_file_allowed(true,true,false,2048u,2047u,512u,8u)) return false;
    if (overwrite_existing_file_allowed(true,true,true,512u,512u,512u,8u)) return false;
    if (overwrite_existing_file_allowed(false,true,false,512u,512u,512u,8u)) return false;
    if (!gpt_backup_header_relationship(1000u,1u,1000u,1000u,1u,
            34u,966u,34u,966u,128u,128u,128u,128u,0x12345678u,0x12345678u,true)) return false;
    if (gpt_backup_header_relationship(1000u,1u,1000u,999u,1u,
            34u,966u,34u,966u,128u,128u,128u,128u,7u,7u,true)) return false;
    if (gpt_backup_header_relationship(1000u,1u,1000u,1000u,1u,
            34u,966u,34u,966u,128u,128u,128u,128u,7u,8u,true)) return false;
    if (gpt_backup_header_relationship(1000u,1u,1000u,1000u,1u,
            34u,966u,34u,965u,128u,128u,128u,128u,7u,7u,true)) return false;
    return true;
}
}
