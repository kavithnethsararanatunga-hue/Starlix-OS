/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#include "storage.hpp"
#include "../drivers/ehci.hpp"
#include "../drivers/serial.hpp"
#include "../drivers/ehci_storage_policy.hpp"
#include "../drivers/ata_pio.hpp"

namespace starlix::storage {
namespace {

static constexpr uint32_t kMaxFatChainWalk = 65536u;
static constexpr size_t kMaxRxfsVolumes = kMaxBlockDevices;
alignas(4096) static uint8_t sector_scratch[kMaxLogicalBlockBytes];
alignas(4096) static uint8_t sector_scratch_copy[kMaxLogicalBlockBytes];
static BlockDevice block_devices[kMaxBlockDevices]{};
static size_t registered_device_count = 0;
static Partition partition_scratch[kMaxPartitionsPerDevice]{};
static Fat32Volume mounted_volumes[kMaxBlockDevices]{};
static size_t mounted_volume_count = 0;
static RxfsVolume mounted_rxfs_volumes[kMaxRxfsVolumes]{};
static size_t mounted_rxfs_volume_count = 0;

struct RxfsBlockIo {
    BlockDevice device;
    Partition partition;
    uint64_t total_blocks;
    uint32_t device_blocks_per_rxfs_block;
};

static bool rxfs_storage_read_block(void* opaque, uint64_t block_lba,
                                    void* output, uint32_t output_capacity) {
    auto* io = static_cast<RxfsBlockIo*>(opaque);
    if (!io || !output || output_capacity < starlix::rxfs::RXFS_BLOCK_SIZE ||
        !io->device.present || io->device_blocks_per_rxfs_block == 0u ||
        block_lba >= io->total_blocks) return false;

    const uint64_t relative_lba =
        block_lba * io->device_blocks_per_rxfs_block;
    const uint64_t sector_count = io->device_blocks_per_rxfs_block;
    const uint64_t partition_sectors = io->partition.sector_count != 0u
        ? io->partition.sector_count
        : (static_cast<uint64_t>(io->device.last_lba) + 1u - io->partition.first_lba);
    if (relative_lba > partition_sectors ||
        sector_count > partition_sectors - relative_lba) return false;
    const uint64_t absolute_lba = io->partition.first_lba + relative_lba;
    if (absolute_lba > io->device.last_lba ||
        sector_count > static_cast<uint64_t>(io->device.last_lba) + 1u - absolute_lba ||
        sector_count > 0xFFFFu) return false;

    return block_read(io->device, absolute_lba,
                      static_cast<uint16_t>(sector_count), output,
                      static_cast<uint32_t>(starlix::rxfs::RXFS_BLOCK_SIZE));
}

static bool rxfs_prepare_io(const BlockDevice& device, const Partition& partition,
                            RxfsBlockIo* output) {
    if (!output || !partition.present || !device.present ||
        device.block_size < 512u || device.block_size > starlix::rxfs::RXFS_BLOCK_SIZE ||
        starlix::rxfs::RXFS_BLOCK_SIZE % device.block_size != 0u ||
        partition.first_lba > device.last_lba) return false;
    const uint32_t ratio = static_cast<uint32_t>(starlix::rxfs::RXFS_BLOCK_SIZE / device.block_size);
    const uint64_t available_sectors = partition.sector_count != 0u
        ? partition.sector_count
        : (static_cast<uint64_t>(device.last_lba) + 1u - partition.first_lba);
    const uint64_t total_blocks = available_sectors / ratio;
    if (total_blocks < 2u) return false;
    output->device = device;
    output->partition = partition;
    output->total_blocks = total_blocks;
    output->device_blocks_per_rxfs_block = ratio;
    return true;
}

static bool rxfs_try_mount(const BlockDevice& device, const Partition& partition,
                           RxfsVolume* output) {
    if (!output) return false;
    *output = {};
    RxfsBlockIo io{};
    if (!rxfs_prepare_io(device, partition, &io)) return false;
    starlix::kernel::rxfs::NativeRxfsContext context{};
    if (!starlix::kernel::rxfs::MountNativeRxfs(
            &io, rxfs_storage_read_block, io.total_blocks, &context)) return false;
    output->mounted = true;
    output->device = device;
    output->partition = partition;
    output->total_blocks = context.superblock.total_blocks;
    output->superblock = context.superblock;
    return true;
}

static bool rxfs_make_context(const RxfsVolume& volume, RxfsBlockIo* io,
                              starlix::kernel::rxfs::NativeRxfsContext* context) {
    if (!volume.mounted || !io || !context ||
        !rxfs_prepare_io(volume.device, volume.partition, io) ||
        volume.total_blocks == 0u || volume.total_blocks > io->total_blocks ||
        !starlix::kernel::rxfs::rxfs_superblock_valid(
            volume.superblock, volume.total_blocks)) return false;
    context->io_context = io;
    context->read_block = rxfs_storage_read_block;
    context->volume_blocks = volume.total_blocks;
    context->superblock = volume.superblock;
    context->valid = true;
    return true;
}

static void copy_bytes(void* destination, const void* source, size_t count) {
    auto* d = static_cast<uint8_t*>(destination);
    const auto* s = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
}

static bool bytes_equal(const uint8_t* a, const char* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != static_cast<uint8_t>(b[i])) return false;
    }
    return true;
}

static uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8u);
}

static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

static uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) |
           (static_cast<uint64_t>(le32(p + 4)) << 32u);
}

static void print_dec(uint64_t value) {
    char digits[21];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u) {
        char item[2] = {digits[--count], '\0'};
        starlix::drivers::serial::print(item);
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static bool read_sector(const BlockDevice& device, uint64_t lba, uint8_t* output) {
    return block_read(device, lba, 1u,
                      output, kMaxLogicalBlockBytes);
}

static bool guid_is_zero(const uint8_t* guid) {
    for (size_t i = 0; i < 16; ++i) if (guid[i] != 0) return false;
    return true;
}

static bool validate_gpt_backup_header(const BlockDevice& device,
                                       uint64_t primary_current_lba,
                                       uint64_t primary_backup_lba,
                                       uint64_t primary_first_usable,
                                       uint64_t primary_last_usable,
                                       uint32_t primary_entry_count,
                                       uint32_t primary_entry_size,
                                       uint32_t primary_entries_crc,
                                       const uint8_t primary_disk_guid[16]) {
    if (!primary_disk_guid || primary_backup_lba != device.last_lba ||
        primary_backup_lba <= primary_last_usable) return false;
    if (!read_sector(device, primary_backup_lba, sector_scratch) ||
        !bytes_equal(sector_scratch, "EFI PART", 8u)) return false;
    const uint32_t header_size = le32(sector_scratch + 12u);
    const uint32_t header_crc = le32(sector_scratch + 16u);
    const uint64_t current_lba = le64(sector_scratch + 24u);
    const uint64_t backup_lba = le64(sector_scratch + 32u);
    const uint64_t first_usable = le64(sector_scratch + 40u);
    const uint64_t last_usable = le64(sector_scratch + 48u);
    const uint64_t entries_lba = le64(sector_scratch + 72u);
    const uint32_t entry_count = le32(sector_scratch + 80u);
    const uint32_t entry_size = le32(sector_scratch + 84u);
    const uint32_t entries_crc = le32(sector_scratch + 88u);
    if (header_size < 92u || header_size > device.block_size ||
        entry_count == 0u || entry_count > 256u || entry_size < 128u ||
        entry_size > device.block_size || device.block_size % entry_size != 0u)
        return false;
    const bool guid_match = [&]() {
        for (size_t i=0;i<16u;++i) if (sector_scratch[56u+i] != primary_disk_guid[i]) return false;
        return true;
    }();
    if (!starlix::drivers::ehci::storage_policy::gpt_backup_header_relationship(
            device.last_lba, primary_current_lba, primary_backup_lba,
            current_lba, backup_lba, primary_first_usable, primary_last_usable,
            first_usable, last_usable, primary_entry_count, primary_entry_size,
            entry_count, entry_size, primary_entries_crc, entries_crc, guid_match))
        return false;
    copy_bytes(sector_scratch_copy, sector_scratch, header_size);
    sector_scratch_copy[16]=0u; sector_scratch_copy[17]=0u;
    sector_scratch_copy[18]=0u; sector_scratch_copy[19]=0u;
    const uint32_t computed_header_crc =
        crc32_update(0xFFFFFFFFu, sector_scratch_copy, header_size) ^ 0xFFFFFFFFu;
    if (computed_header_crc != header_crc) return false;
    const uint64_t entries_bytes = static_cast<uint64_t>(entry_count) * entry_size;
    const uint64_t entries_sectors = (entries_bytes + device.block_size - 1u) / device.block_size;
    if (entries_sectors == 0u || entries_lba <= last_usable ||
        entries_lba >= current_lba || entries_sectors > current_lba - entries_lba ||
        entries_lba + entries_sectors > static_cast<uint64_t>(device.last_lba) + 1u)
        return false;
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t consumed = 0u;
    for (uint64_t i=0u;i<entries_sectors;++i) {
        if (!read_sector(device, entries_lba+i, sector_scratch)) return false;
        size_t used = device.block_size;
        if (entries_bytes-consumed < used) used = static_cast<size_t>(entries_bytes-consumed);
        crc = crc32_update(crc, sector_scratch, used); consumed += used;
    }
    return (crc ^ 0xFFFFFFFFu) == entries_crc;
}

static bool scan_gpt(const BlockDevice& device, Partition* output,
                     size_t output_capacity, size_t* output_count) {
    if (device.block_size < 512u || device.block_size > kMaxLogicalBlockBytes ||
        device.block_size % 128u != 0u || device.last_lba < 1u) return false;
    if (!read_sector(device, 1u, sector_scratch)) return false;
    if (!bytes_equal(sector_scratch, "EFI PART", 8u)) return false;

    const uint32_t header_size = le32(sector_scratch + 12u);
    const uint32_t stored_header_crc = le32(sector_scratch + 16u);
    const uint64_t current_lba = le64(sector_scratch + 24u);
    const uint64_t backup_lba = le64(sector_scratch + 32u);
    const uint64_t first_usable = le64(sector_scratch + 40u);
    const uint64_t last_usable = le64(sector_scratch + 48u);
    const uint64_t entries_lba = le64(sector_scratch + 72u);
    const uint32_t entry_count = le32(sector_scratch + 80u);
    const uint32_t entry_size = le32(sector_scratch + 84u);
    const uint32_t stored_entries_crc = le32(sector_scratch + 88u);

    if (header_size < 92u || header_size > device.block_size || current_lba != 1u ||
        backup_lba != device.last_lba || first_usable > last_usable ||
        last_usable > device.last_lba || entries_lba < 2u ||
        entry_count == 0u || entry_count > 256u ||
        entry_size < 128u || entry_size > device.block_size ||
        (device.block_size % entry_size) != 0u) return false;

    copy_bytes(sector_scratch_copy, sector_scratch, header_size);
    sector_scratch_copy[16] = 0;
    sector_scratch_copy[17] = 0;
    sector_scratch_copy[18] = 0;
    sector_scratch_copy[19] = 0;
    uint32_t crc = crc32_update(0xFFFFFFFFu, sector_scratch_copy, header_size) ^ 0xFFFFFFFFu;
    if (crc != stored_header_crc) return false;
    uint8_t primary_disk_guid[16]{};
    copy_bytes(primary_disk_guid, sector_scratch + 56u, sizeof(primary_disk_guid));

    const uint64_t entries_bytes = static_cast<uint64_t>(entry_count) * entry_size;
    const uint64_t entries_sectors = (entries_bytes + device.block_size - 1u) / device.block_size;
    if (entries_sectors == 0u || entries_lba > first_usable ||
        entries_sectors > first_usable - entries_lba ||
        entries_lba + entries_sectors > static_cast<uint64_t>(device.last_lba) + 1u) return false;

    crc = 0xFFFFFFFFu;
    uint64_t consumed = 0;
    for (uint64_t s = 0; s < entries_sectors; ++s) {
        if (!read_sector(device, entries_lba + s, sector_scratch)) return false;
        size_t use = device.block_size;
        if (entries_bytes - consumed < use) use = static_cast<size_t>(entries_bytes - consumed);
        crc = crc32_update(crc, sector_scratch, use);
        consumed += use;
    }
    crc ^= 0xFFFFFFFFu;
    if (crc != stored_entries_crc) return false;
    if (!validate_gpt_backup_header(device, current_lba, backup_lba,
            first_usable, last_usable, entry_count, entry_size,
            stored_entries_crc, primary_disk_guid)) return false;

    size_t found = 0;
    uint64_t cached_sector = ~static_cast<uint64_t>(0);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint64_t byte_offset = static_cast<uint64_t>(i) * entry_size;
        const uint64_t table_sector = entries_lba + (byte_offset / device.block_size);
        const uint32_t in_sector = static_cast<uint32_t>(byte_offset % device.block_size);
        if (table_sector != cached_sector) {
            if (!read_sector(device, table_sector, sector_scratch)) return false;
            cached_sector = table_sector;
        }
        const uint8_t* entry = sector_scratch + in_sector;
        if (guid_is_zero(entry)) continue;
        const uint64_t first = le64(entry + 32u);
        const uint64_t last = le64(entry + 40u);
        if (first < first_usable || last > last_usable || first > last ||
            last > device.last_lba) continue;
        if (found < output_capacity) {
            Partition& p = output[found];
            p = {};
            p.present = true;
            p.gpt = true;
            p.index = static_cast<uint8_t>((i < 255u) ? i + 1u : 255u);
            p.first_lba = first;
            p.sector_count = last - first + 1u;
            copy_bytes(p.type_guid, entry, 16u);
            ++found;
        }
    }
    *output_count = found;
    return true;
}

static bool partition_table_is_protective_mbr(const uint8_t* mbr) {
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t* e = mbr + 446u + i * 16u;
        if (e[4] == 0xEEu) return true;
    }
    return false;
}

static bool is_fat32_mbr_type(uint8_t type) {
    return type == 0x0Bu || type == 0x0Cu || type == 0x1Bu || type == 0x1Cu;
}

static bool power_of_two(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

static bool fat32_read_sector(const Fat32Volume& volume, uint32_t volume_sector,
                              uint8_t* output) {
    if (!volume.mounted || volume_sector >= volume.total_sectors) return false;
    const uint64_t absolute = volume.partition.first_lba + volume_sector;
    if (absolute > volume.device.last_lba) return false;
    return read_sector(volume.device, absolute, output);
}

static bool fat32_next_cluster(const Fat32Volume& volume, uint32_t cluster,
                               uint32_t* next_cluster) {
    if (!next_cluster || cluster < 2u || cluster >= volume.cluster_count + 2u) return false;
    const uint64_t fat_byte = static_cast<uint64_t>(cluster) * 4u;
    const uint32_t fat_sector = static_cast<uint32_t>(fat_byte / volume.bytes_per_sector);
    const uint32_t fat_offset = static_cast<uint32_t>(fat_byte % volume.bytes_per_sector);
    const uint64_t fat_base = volume.partition.first_lba + volume.reserved_sectors +
        static_cast<uint64_t>(volume.active_fat) * volume.sectors_per_fat;
    if (fat_sector >= volume.sectors_per_fat || fat_base + fat_sector > volume.device.last_lba)
        return false;
    if (!read_sector(volume.device, fat_base + fat_sector, sector_scratch)) return false;
    const uint32_t value = le32(sector_scratch + fat_offset) & 0x0FFFFFFFu;
    if (value >= 0x0FFFFFF8u) {
        *next_cluster = 0x0FFFFFFFu;
        return true;
    }
    if (value < 2u || value >= volume.cluster_count + 2u) return false;
    *next_cluster = value;
    return true;
}

static uint64_t fat32_cluster_lba(const Fat32Volume& volume, uint32_t cluster) {
    return volume.partition.first_lba + volume.first_data_sector +
        static_cast<uint64_t>(cluster - 2u) * volume.sectors_per_cluster;
}

static void make_short_name(const uint8_t* entry, char* output, size_t capacity) {
    if (capacity == 0) return;
    size_t at = 0;
    for (size_t i = 0; i < 8u && entry[i] != ' '; ++i) {
        if (at + 1u >= capacity) break;
        uint8_t c = entry[i];
        output[at++] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : '?';
    }
    size_t ext = 3;
    while (ext > 0u && entry[8u + ext - 1u] == ' ') --ext;
    if (ext > 0u && at + 1u < capacity) output[at++] = '.';
    for (size_t i = 0; i < ext && at + 1u < capacity; ++i) {
        uint8_t c = entry[8u + i];
        output[at++] = (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : '?';
    }
    output[at] = '\0';
}

static bool name_to_83(const char* name, uint8_t output[11]) {
    for (size_t i = 0; i < 11; ++i) output[i] = ' ';
    if (!name || !*name) return false;
    size_t base = 0, ext = 0;
    bool after_dot = false;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        if (c == '.') {
            if (after_dot || base == 0) return false;
            after_dot = true;
            continue;
        }
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c < 0x21 || c > 0x7E || c == '/' || c == '\\') return false;
        if (!after_dot) {
            if (base >= 8u) return false;
            output[base++] = static_cast<uint8_t>(c);
        } else {
            if (ext >= 3u) return false;
            output[8u + ext++] = static_cast<uint8_t>(c);
        }
    }
    return base != 0u;
}

struct FatFileEntry {
    bool found;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attributes;
    char name[769]; /* 255 UTF-16 units encoded as UTF-8, plus NUL */
};

struct LfnState {
    bool active;
    bool valid;
    uint8_t expected_ordinal;
    uint8_t checksum;
    uint16_t units[260];
};

static uint8_t fat_lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 11u; ++i)
        sum = static_cast<uint8_t>(((sum & 1u) ? 0x80u : 0u) + (sum >> 1u) + short_name[i]);
    return sum;
}

static void lfn_reset(LfnState& state) {
    state.active = false;
    state.valid = false;
    state.expected_ordinal = 0;
    state.checksum = 0;
    for (size_t i = 0; i < 260u; ++i) state.units[i] = 0xFFFFu;
}

static void lfn_consume_entry(LfnState& state, const uint8_t* entry) {
    const uint8_t raw_ordinal = entry[0];
    const uint8_t ordinal = raw_ordinal & 0x1Fu;
    if ((raw_ordinal & 0xA0u) != 0u || ordinal == 0u || ordinal > 20u ||
        entry[11] != 0x0Fu || entry[12] != 0u || le16(entry + 26u) != 0u) {
        lfn_reset(state);
        return;
    }
    if ((raw_ordinal & 0x40u) != 0u) {
        lfn_reset(state);
        state.active = true;
        state.valid = true;
        state.expected_ordinal = ordinal;
        state.checksum = entry[13];
    }
    if (!state.active || !state.valid || ordinal != state.expected_ordinal ||
        entry[13] != state.checksum) {
        lfn_reset(state);
        return;
    }

    static constexpr uint8_t kOffsets[13] = {
        1u, 3u, 5u, 7u, 9u, 14u, 16u, 18u, 20u, 22u, 24u, 28u, 30u
    };
    const size_t base = static_cast<size_t>(ordinal - 1u) * 13u;
    for (size_t i = 0; i < 13u; ++i) {
        const uint16_t unit = le16(entry + kOffsets[i]);
        const size_t at = base + i;
        if (at >= 255u && unit != 0u && unit != 0xFFFFu) {
            lfn_reset(state);
            return;
        }
        state.units[at] = unit;
    }
    state.expected_ordinal = static_cast<uint8_t>(ordinal - 1u);
}

static bool lfn_to_utf8(const LfnState& state, const uint8_t short_name[11], char output[769]) {
    output[0] = '\0';
    if (!state.active || !state.valid || state.expected_ordinal != 0u ||
        state.checksum != fat_lfn_checksum(short_name)) return false;

    size_t out = 0;
    bool ended = false;
    for (size_t i = 0; i < 255u && out < 768u; ++i) {
        uint32_t cp = state.units[i];
        if (cp == 0u || cp == 0xFFFFu) { ended = true; break; }
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            const uint16_t low = state.units[i + 1u];
            if (i + 1u < 255u && low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
                ++i;
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        if (cp < 0x80u) {
            output[out++] = static_cast<char>(cp);
        } else if (cp < 0x800u) {
            output[out++] = static_cast<char>(0xC0u | (cp >> 6u));
            output[out++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            output[out++] = static_cast<char>(0xE0u | (cp >> 12u));
            output[out++] = static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
            output[out++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            output[out++] = static_cast<char>(0xF0u | (cp >> 18u));
            output[out++] = static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu));
            output[out++] = static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
            output[out++] = static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }
    (void)ended;
    output[out] = '\0';
    return out != 0u;
}

static bool names_equal_ascii_case_insensitive(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'a' && ca <= 'z') ca = static_cast<unsigned char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<unsigned char>(cb - 'a' + 'A');
        if (ca != cb) return false;
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void fat32_entry_name(const LfnState& lfn, const uint8_t* entry, char output[769]) {
    if (!lfn_to_utf8(lfn, entry, output)) make_short_name(entry, output, 769u);
}

static bool fat32_find_root_file(const Fat32Volume& volume, const uint8_t wanted[11],
                                 FatFileEntry* result) {
    if (!result) return false;
    result->found = false;
    uint32_t cluster = volume.root_cluster;
    for (uint32_t depth = 0; depth < volume.cluster_count && depth < kMaxFatChainWalk; ++depth) {
        const uint64_t first_sector = fat32_cluster_lba(volume, cluster);
        for (uint32_t s = 0; s < volume.sectors_per_cluster; ++s) {
            if (!fat32_read_sector(volume, static_cast<uint32_t>(first_sector - volume.partition.first_lba + s), sector_scratch))
                return false;
            for (uint32_t offset = 0; offset + 32u <= volume.bytes_per_sector; offset += 32u) {
                const uint8_t* entry = sector_scratch + offset;
                if (entry[0] == 0x00u) return true;
                if (entry[0] == 0xE5u || entry[11] == 0x0Fu || (entry[11] & 0x08u) != 0u)
                    continue;
                bool match = true;
                for (size_t i = 0; i < 11u; ++i) if (entry[i] != wanted[i]) match = false;
                if (!match) continue;
                result->found = true;
                result->attributes = entry[11];
                result->first_cluster = (static_cast<uint32_t>(le16(entry + 20u)) << 16u) |
                                        le16(entry + 26u);
                result->size = le32(entry + 28u);
                return true;
            }
        }
        uint32_t next = 0;
        if (!fat32_next_cluster(volume, cluster, &next)) return false;
        if (next >= 0x0FFFFFF8u) return true;
        cluster = next;
    }
    return false;
}

static bool fat32_find_directory_entry(const Fat32Volume& volume, uint32_t directory_cluster,
                                       const char* wanted, FatFileEntry* result) {
    if (!result || !wanted || !*wanted || directory_cluster < 2u ||
        directory_cluster >= volume.cluster_count + 2u) return false;
    *result = {};
    uint32_t cluster = directory_cluster;
    LfnState lfn{};
    lfn_reset(lfn);
    for (uint32_t depth = 0; depth < volume.cluster_count && depth < kMaxFatChainWalk; ++depth) {
        if (cluster < 2u || cluster >= volume.cluster_count + 2u) return false;
        const uint64_t first_sector = volume.first_data_sector +
            static_cast<uint64_t>(cluster - 2u) * volume.sectors_per_cluster;
        for (uint32_t s = 0; s < volume.sectors_per_cluster; ++s) {
            const uint64_t local_sector = first_sector + s;
            if (local_sector > 0xFFFFFFFFu ||
                !fat32_read_sector(volume, static_cast<uint32_t>(local_sector), sector_scratch))
                return false;
            for (uint32_t offset = 0; offset + 32u <= volume.bytes_per_sector; offset += 32u) {
                const uint8_t* entry = sector_scratch + offset;
                if (entry[0] == 0x00u) return true; /* clean end: result remains not-found */
                if (entry[0] == 0xE5u) { lfn_reset(lfn); continue; }
                if (entry[11] == 0x0Fu) { lfn_consume_entry(lfn, entry); continue; }
                if ((entry[11] & 0x08u) != 0u) { lfn_reset(lfn); continue; }

                FatFileEntry candidate{};
                candidate.attributes = entry[11];
                candidate.first_cluster = (static_cast<uint32_t>(le16(entry + 20u)) << 16u) |
                                          le16(entry + 26u);
                candidate.size = le32(entry + 28u);
                fat32_entry_name(lfn, entry, candidate.name);
                lfn_reset(lfn);
                if (names_equal_ascii_case_insensitive(candidate.name, wanted)) {
                    candidate.found = true;
                    *result = candidate;
                    return true;
                }
            }
        }
        uint32_t next = 0;
        if (!fat32_next_cluster(volume, cluster, &next)) return false;
        if (next >= 0x0FFFFFF8u) return true;
        cluster = next;
    }
    return false;
}

static bool fat32_resolve_path(const Fat32Volume& volume, const char* path,
                               FatFileEntry* result) {
    if (!result || !volume.mounted || !path) return false;
    *result = {};
    const char* cursor = path;
    while (*cursor == '/' || *cursor == '\\') ++cursor;
    if (*cursor == '\0') return true;

    uint32_t directory_cluster = volume.root_cluster;
    for (;;) {
        char component[769]{};
        size_t length = 0;
        while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
            if (length + 1u >= sizeof(component)) return false;
            component[length++] = *cursor++;
        }
        component[length] = '\0';
        while (*cursor == '/' || *cursor == '\\') ++cursor;
        const bool final_component = (*cursor == '\0');
        if (length == 0u) return false;

        FatFileEntry found{};
        if (!fat32_find_directory_entry(volume, directory_cluster, component, &found)) return false;
        if (!found.found) return true;
        if (final_component) {
            *result = found;
            return true;
        }
        if ((found.attributes & 0x10u) == 0u || found.first_cluster < 2u ||
            found.first_cluster >= volume.cluster_count + 2u) return true;
        directory_cluster = found.first_cluster;
    }
}

static bool fat32_list_cluster(const Fat32Volume& volume, uint32_t start_cluster, bool root_listing) {
    if (!volume.mounted || start_cluster < 2u || start_cluster >= volume.cluster_count + 2u) return false;
    uint32_t cluster = start_cluster;
    size_t emitted = 0;
    LfnState lfn{};
    lfn_reset(lfn);
    for (uint32_t depth = 0; depth < volume.cluster_count && depth < kMaxFatChainWalk; ++depth) {
        if (cluster < 2u || cluster >= volume.cluster_count + 2u) return false;
        const uint64_t first_sector = volume.first_data_sector +
            static_cast<uint64_t>(cluster - 2u) * volume.sectors_per_cluster;
        bool end_of_directory = false;
        for (uint32_t s = 0; s < volume.sectors_per_cluster; ++s) {
            const uint64_t local_sector = first_sector + s;
            if (local_sector > 0xFFFFFFFFu ||
                !fat32_read_sector(volume, static_cast<uint32_t>(local_sector), sector_scratch))
                return false;
            for (uint32_t offset = 0; offset + 32u <= volume.bytes_per_sector; offset += 32u) {
                const uint8_t* entry = sector_scratch + offset;
                if (entry[0] == 0u) { end_of_directory = true; break; }
                if (entry[0] == 0xE5u) { lfn_reset(lfn); continue; }
                if (entry[11] == 0x0Fu) { lfn_consume_entry(lfn, entry); continue; }
                if ((entry[11] & 0x08u) != 0u) { lfn_reset(lfn); continue; }
                char name[769]{};
                fat32_entry_name(lfn, entry, name);
                lfn_reset(lfn);
                starlix::drivers::serial::print(root_listing ? "[FAT32] root entry: " : "[FAT32] directory entry: ");
                starlix::drivers::serial::print(name);
                if ((entry[11] & 0x10u) == 0u) {
                    starlix::drivers::serial::print(" (");
                    print_dec(le32(entry + 28u));
                    starlix::drivers::serial::print(" bytes)");
                } else {
                    starlix::drivers::serial::print(" <DIR>");
                }
                starlix::drivers::serial::print("\n");
                if (++emitted >= 64u) {
                    starlix::drivers::serial::print("[FAT32] Directory listing limited to 64 entries.\n");
                    return true;
                }
            }
            if (end_of_directory) return true;
        }
        uint32_t next = 0;
        if (!fat32_next_cluster(volume, cluster, &next)) return false;
        if (next >= 0x0FFFFFF8u) return true;
        cluster = next;
    }
    return false;
}

static bool fat32_read_entry_data(const Fat32Volume& volume, const FatFileEntry& file,
                                  void* output, uint32_t output_capacity, uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!volume.mounted || !file.found || !output || !bytes_read ||
        (file.attributes & 0x10u) != 0u || file.size > output_capacity) return false;
    if (file.size == 0u) return true;
    if (file.first_cluster < 2u || file.first_cluster >= volume.cluster_count + 2u) return false;
    auto* out = static_cast<uint8_t*>(output);
    uint32_t remaining = file.size;
    uint32_t cluster = file.first_cluster;
    for (uint32_t depth = 0; depth < volume.cluster_count && depth < kMaxFatChainWalk && remaining != 0u; ++depth) {
        if (cluster < 2u || cluster >= volume.cluster_count + 2u) return false;
        const uint64_t first_sector = volume.first_data_sector +
            static_cast<uint64_t>(cluster - 2u) * volume.sectors_per_cluster;
        for (uint32_t s = 0; s < volume.sectors_per_cluster && remaining != 0u; ++s) {
            const uint64_t local_sector = first_sector + s;
            if (local_sector > 0xFFFFFFFFu ||
                !fat32_read_sector(volume, static_cast<uint32_t>(local_sector), sector_scratch))
                return false;
            uint32_t amount = remaining < volume.bytes_per_sector ? remaining : volume.bytes_per_sector;
            copy_bytes(out + *bytes_read, sector_scratch, amount);
            *bytes_read += amount;
            remaining -= amount;
        }
        if (remaining == 0u) return *bytes_read == file.size;
        uint32_t next = 0;
        if (!fat32_next_cluster(volume, cluster, &next) || next >= 0x0FFFFFF8u) return false;
        cluster = next;
    }
    return false;
}

static bool fat32_overwrite_preflight(const Fat32Volume& volume,
                                      const FatFileEntry& file) {
    if (!volume.mounted || !file.found || (file.attributes & 0x10u) != 0u ||
        file.size > starlix::drivers::ehci::storage_policy::kMaxInPlaceOverwriteBytes ||
        volume.bytes_per_sector == 0u || volume.sectors_per_cluster == 0u) return false;
    if (file.size == 0u) return true;
    const uint64_t bytes_per_cluster = static_cast<uint64_t>(volume.bytes_per_sector) * volume.sectors_per_cluster;
    if (bytes_per_cluster == 0u) return false;
    const uint64_t needed64 = (static_cast<uint64_t>(file.size) + bytes_per_cluster - 1u) / bytes_per_cluster;
    if (needed64 == 0u || needed64 > volume.cluster_count || needed64 > 128u) return false;
    /* Existing-file overwrite is deliberately capped at 64KiB. Record visited clusters
       to reject cycles before the first write; storage APIs require serialized callers. */
    static uint32_t seen_clusters[128]{};
    uint32_t cluster = file.first_cluster;
    for (uint64_t i=0u;i<needed64;++i) {
        if (cluster < 2u || cluster >= volume.cluster_count + 2u) return false;
        for (uint64_t j=0u;j<i;++j) if (seen_clusters[j] == cluster) return false;
        seen_clusters[i]=cluster;
        const uint64_t local_first = volume.first_data_sector +
            static_cast<uint64_t>(cluster - 2u) * volume.sectors_per_cluster;
        if (local_first > volume.total_sectors || volume.sectors_per_cluster > volume.total_sectors-local_first) return false;
        const uint64_t absolute = volume.partition.first_lba + local_first;
        if (absolute > volume.device.last_lba ||
            volume.sectors_per_cluster > static_cast<uint64_t>(volume.device.last_lba)+1u-absolute) return false;
        if (i+1u<needed64) {
            uint32_t next=0u;
            if (!fat32_next_cluster(volume,cluster,&next) || next >= 0x0FFFFFF8u) return false;
            cluster=next;
        }
    }
    return true;
}

static bool fat32_try_mount(const BlockDevice& device, const Partition& partition,
                            Fat32Volume* output) {
    if (!output || !partition.present || device.block_size < 512u ||
        device.block_size > kMaxLogicalBlockBytes || partition.first_lba > device.last_lba)
        return false;
    if (!read_sector(device, partition.first_lba, sector_scratch)) return false;
    const uint32_t bps = le16(sector_scratch + 11u);
    const uint32_t spc = sector_scratch[13];
    const uint32_t reserved = le16(sector_scratch + 14u);
    const uint32_t fats = sector_scratch[16];
    const uint32_t total16 = le16(sector_scratch + 19u);
    const uint32_t fat16 = le16(sector_scratch + 22u);
    const uint32_t total32 = le32(sector_scratch + 32u);
    const uint32_t fat32 = le32(sector_scratch + 36u);
    const uint16_t ext_flags = le16(sector_scratch + 40u);
    const uint16_t fs_version = le16(sector_scratch + 42u);
    const uint32_t root = le32(sector_scratch + 44u) & 0x0FFFFFFFu;
    const uint32_t total = total16 != 0u ? total16 : total32;
    if ((bps != 512u && bps != 1024u && bps != 2048u && bps != 4096u) ||
        bps != device.block_size) return false;
    if (sector_scratch[bps - 2u] != 0x55u || sector_scratch[bps - 1u] != 0xAAu) return false;
    if (!power_of_two(spc) || spc > 128u ||
        reserved == 0u || fats == 0u || fats > 2u || fat16 != 0u || fat32 == 0u ||
        fs_version != 0u || root < 2u || total == 0u ||
        (partition.sector_count != 0u && total > partition.sector_count) ||
        total > static_cast<uint64_t>(device.last_lba) + 1u - partition.first_lba) return false;
    const uint64_t metadata_sectors = static_cast<uint64_t>(reserved) +
                                      static_cast<uint64_t>(fats) * fat32;
    if (metadata_sectors >= total) return false;
    const uint32_t data_sectors = total - static_cast<uint32_t>(metadata_sectors);
    const uint32_t clusters = data_sectors / spc;
    if (clusters == 0u || root >= clusters + 2u ||
        static_cast<uint64_t>(fat32) * bps / 4u < static_cast<uint64_t>(clusters) + 2u)
        return false;
    uint32_t active_fat = 0;
    if ((ext_flags & 0x0080u) != 0u) {
        active_fat = ext_flags & 0x000Fu;
        if (active_fat >= fats) return false;
    }
    Fat32Volume volume{};
    volume.mounted = true;
    volume.device = device;
    volume.partition = partition;
    volume.bytes_per_sector = bps;
    volume.sectors_per_cluster = spc;
    volume.reserved_sectors = reserved;
    volume.fat_count = fats;
    volume.sectors_per_fat = fat32;
    volume.root_cluster = root;
    volume.first_data_sector = static_cast<uint32_t>(metadata_sectors);
    volume.total_sectors = total;
    volume.cluster_count = clusters;
    volume.active_fat = active_fat;
    *output = volume;
    return true;
}

static void log_volume(const Fat32Volume& volume) {
    starlix::drivers::serial::print("[FAT32] Mounted read-only volume; sector size ");
    print_dec(volume.bytes_per_sector);
    starlix::drivers::serial::print(", cluster size ");
    print_dec(static_cast<uint64_t>(volume.bytes_per_sector) * volume.sectors_per_cluster);
    starlix::drivers::serial::print(" bytes.\n");
}

static void smoke_test_mounted_volume(const Fat32Volume& volume) {
    /* Read-only smoke tests run for partitioned volumes as well as superfloppies. */
    alignas(16) static uint8_t smoke_buffer[4096];
    uint32_t amount = 0;
    if (fat32_read_file_83(volume, "HELLO.TXT", smoke_buffer,
                           sizeof(smoke_buffer), &amount)) {
        starlix::drivers::serial::print("[FAT32] HELLO.TXT read smoke test passed; bytes read: ");
        print_dec(amount);
        starlix::drivers::serial::print(".\n");
    }
    amount = 0;
    if (fat32_read_file(volume, "Long File Name.txt", smoke_buffer,
                        sizeof(smoke_buffer), &amount)) {
        starlix::drivers::serial::print("[FAT32] Long File Name.txt read smoke test passed; bytes read: ");
        print_dec(amount);
        starlix::drivers::serial::print(".\n");
    }
    amount = 0;
    if (fat32_read_file(volume, "Docs/Nested Note.txt", smoke_buffer,
                        sizeof(smoke_buffer), &amount)) {
        starlix::drivers::serial::print("[FAT32] Docs/Nested Note.txt read smoke test passed; bytes read: ");
        print_dec(amount);
        starlix::drivers::serial::print(".\n");
    }
    (void)fat32_list_directory(volume, "Docs");
}

static void try_mount_partitions(const BlockDevice& device, const Partition* partitions,
                                 size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (mounted_rxfs_volume_count < kMaxRxfsVolumes) {
            RxfsVolume rxfs_volume{};
            if (rxfs_try_mount(device, partitions[i], &rxfs_volume)) {
                mounted_rxfs_volumes[mounted_rxfs_volume_count++] = rxfs_volume;
                starlix::drivers::serial::print("[RXFS] Mounted read-only volume on partition ");
                print_dec(partitions[i].index);
                starlix::drivers::serial::print(" at LBA ");
                print_dec(partitions[i].first_lba);
                starlix::drivers::serial::print(".\n");
            }
        }

        if (!partitions[i].gpt && !is_fat32_mbr_type(partitions[i].mbr_type)) continue;
        if (mounted_volume_count >= kMaxBlockDevices) continue;
        Fat32Volume volume{};
        if (!fat32_try_mount(device, partitions[i], &volume)) continue;
        mounted_volumes[mounted_volume_count++] = volume;
        starlix::drivers::serial::print("[FAT32] Mounted partition ");
        print_dec(partitions[i].index);
        starlix::drivers::serial::print(" at LBA ");
        print_dec(partitions[i].first_lba);
        starlix::drivers::serial::print(".\n");
        log_volume(volume);
        (void)fat32_list_root(volume);
        smoke_test_mounted_volume(volume);
    }
}

} // namespace

size_t block_device_count() { return registered_device_count; }

bool block_device_get(size_t index, BlockDevice* output) {
    if (!output || index >= registered_device_count) return false;
    *output = block_devices[index];
    return true;
}

size_t fat32_volume_count() { return mounted_volume_count; }

size_t rxfs_volume_count() { return mounted_rxfs_volume_count; }

bool rxfs_volume_get(size_t index, RxfsVolume* output) {
    if (!output || index >= mounted_rxfs_volume_count) return false;
    *output = mounted_rxfs_volumes[index];
    return true;
}

bool fat32_volume_get(size_t index, Fat32Volume* output) {
    if (!output || index >= mounted_volume_count) return false;
    *output = mounted_volumes[index];
    return true;
}

bool block_read(const BlockDevice& device, uint64_t lba, uint16_t block_count,
                void* output, uint32_t output_capacity) {
    if (!device.present || !output || block_count == 0u ||
        device.block_size < 512u || device.block_size > kMaxLogicalBlockBytes) return false;
    const uint64_t bytes = static_cast<uint64_t>(block_count) * device.block_size;
    if (bytes > output_capacity || bytes > 4096u || lba > device.last_lba ||
        static_cast<uint64_t>(block_count) > device.last_lba + 1u - lba) return false;

    if (device.backend == BlockBackend::AtaPio) {
        starlix::drivers::ata::AtaPioDeviceInfo ata{};
        ata.present = true;
        ata.channel = device.ata_channel;
        ata.drive = device.ata_drive;
        ata.command_base = device.ata_command_base;
        ata.control_port = device.ata_control_port;
        ata.lba48 = device.ata_lba48;
        ata.last_lba = device.last_lba;
        ata.logical_sector_bytes = device.block_size;
        return starlix::drivers::ata::ata_pio_read_blocks(
            ata, lba, block_count, output, output_capacity);
    }

    if (device.backend != BlockBackend::UsbMassStorage || lba > 0xFFFFFFFFu ||
        static_cast<uint64_t>(block_count) > 0x100000000ull - lba) return false;
    return starlix::drivers::ehci::ehci_msc_read_blocks(
        device.ehci_address, static_cast<uint32_t>(lba), block_count,
        output, output_capacity);
}

bool block_write(const BlockDevice& device, uint32_t lba, uint16_t block_count,
                 const void* input, uint32_t input_capacity) {
    /* ATA PIO is intentionally read-only in this milestone. */
    if (device.backend != BlockBackend::UsbMassStorage ||
        device.last_lba > 0xFFFFFFFFu || !input ||
        !starlix::drivers::ehci::storage_policy::block_range_allowed(
            device.present, device.block_size, static_cast<uint32_t>(device.last_lba), lba,
            block_count, input_capacity)) return false;
    return starlix::drivers::ehci::ehci_msc_write_blocks(
        device.ehci_address, lba, block_count, input, input_capacity);
}

bool block_flush(const BlockDevice& device) {
    if (device.backend != BlockBackend::UsbMassStorage || !device.present ||
        device.block_size < 512u || device.block_size > kMaxLogicalBlockBytes) return false;
    return starlix::drivers::ehci::ehci_msc_synchronize_cache(device.ehci_address);
}

bool partition_scan(const BlockDevice& device, Partition* output,
                    size_t output_capacity, size_t* output_count) {
    if (!output || output_capacity == 0u || !output_count) return false;
    *output_count = 0;
    if (device.block_size < 512u || device.block_size > kMaxLogicalBlockBytes ||
        !read_sector(device, 0u, sector_scratch)) return false;
    if (sector_scratch[510] != 0x55u || sector_scratch[511] != 0xAAu) return true;
    if (partition_table_is_protective_mbr(sector_scratch))
        return scan_gpt(device, output, output_capacity, output_count);

    size_t found = 0;
    for (unsigned i = 0; i < 4u; ++i) {
        const uint8_t* entry = sector_scratch + 446u + i * 16u;
        const uint8_t type = entry[4];
        const uint32_t first = le32(entry + 8u);
        const uint32_t count = le32(entry + 12u);
        if (type == 0u || count == 0u) continue;
        const uint64_t end = static_cast<uint64_t>(first) + count;
        if (first == 0u || end > static_cast<uint64_t>(device.last_lba) + 1u) continue;
        if (found < output_capacity) {
            Partition& p = output[found++];
            p = {};
            p.present = true;
            p.index = static_cast<uint8_t>(i + 1u);
            p.mbr_type = type;
            p.first_lba = first;
            p.sector_count = count;
        }
    }
    *output_count = found;
    return true;
}

bool fat32_mount(const BlockDevice& device, const Partition& partition,
                 Fat32Volume* output) {
    if (!output) return false;
    *output = {};
    return fat32_try_mount(device, partition, output);
}

bool rxfs_mount(const BlockDevice& device, const Partition& partition,
                RxfsVolume* output) {
    if (!output) return false;
    *output = {};
    return rxfs_try_mount(device, partition, output);
}

bool rxfs_lookup_path(const RxfsVolume& volume, const char* path,
                      starlix::kernel::rxfs::NativeRxfsNode* output) {
    if (output) *output = {};
    RxfsBlockIo io{};
    starlix::kernel::rxfs::NativeRxfsContext context{};
    return output && rxfs_make_context(volume, &io, &context) &&
        starlix::kernel::rxfs::LookupNativeRxfsPath(&context, path, output);
}

bool rxfs_read_file(const RxfsVolume& volume, const char* path,
                    void* output, uint32_t output_capacity, uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0u;
    RxfsBlockIo io{};
    starlix::kernel::rxfs::NativeRxfsContext context{};
    return bytes_read && rxfs_make_context(volume, &io, &context) &&
        starlix::kernel::rxfs::ReadNativeRxfsFile(
            &context, path, output, output_capacity, bytes_read);
}

bool fat32_list_root(const Fat32Volume& volume) {
    if (!volume.mounted) return false;
    return fat32_list_cluster(volume, volume.root_cluster, true);
}

bool fat32_list_directory(const Fat32Volume& volume, const char* path) {
    if (!volume.mounted || !path) return false;
    const char* cursor = path;
    while (*cursor == '/' || *cursor == '\\') ++cursor;
    if (*cursor == '\0') return fat32_list_root(volume);
    FatFileEntry entry{};
    if (!fat32_resolve_path(volume, path, &entry) || !entry.found ||
        (entry.attributes & 0x10u) == 0u) return false;
    return fat32_list_cluster(volume, entry.first_cluster, false);
}

bool fat32_read_file_83(const Fat32Volume& volume, const char* name,
                        void* output, uint32_t output_capacity,
                        uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!volume.mounted || !output || !bytes_read) return false;
    uint8_t wanted[11];
    if (!name_to_83(name, wanted)) return false;
    FatFileEntry file{};
    if (!fat32_find_root_file(volume, wanted, &file) || !file.found) return false;
    return fat32_read_entry_data(volume, file, output, output_capacity, bytes_read);
}

bool fat32_read_file(const Fat32Volume& volume, const char* path,
                     void* output, uint32_t output_capacity, uint32_t* bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!volume.mounted || !path || !*path || !output || !bytes_read) return false;
    FatFileEntry file{};
    if (!fat32_resolve_path(volume, path, &file) || !file.found) return false;
    return fat32_read_entry_data(volume, file, output, output_capacity, bytes_read);
}

bool fat32_overwrite_existing_file(const Fat32Volume& volume, const char* path,
                                   const void* data, uint32_t length) {
    if (!volume.mounted || !path || !*path || (length != 0u && !data)) return false;
    FatFileEntry file{};
    if (!fat32_resolve_path(volume, path, &file) || !file.found ||
        !starlix::drivers::ehci::storage_policy::overwrite_existing_file_allowed(
            volume.mounted, file.found, (file.attributes & 0x10u) != 0u,
            file.size, length, volume.bytes_per_sector, volume.cluster_count) ||
        !fat32_overwrite_preflight(volume, file)) return false;
    if (length == 0u) return block_flush(volume.device);

    const auto* source = static_cast<const uint8_t*>(data);
    uint32_t remaining=length, offset=0u, cluster=file.first_cluster;
    const uint64_t bytes_per_cluster=static_cast<uint64_t>(volume.bytes_per_sector)*volume.sectors_per_cluster;
    const uint64_t clusters_needed=(static_cast<uint64_t>(length)+bytes_per_cluster-1u)/bytes_per_cluster;
    for (uint64_t ci=0u;ci<clusters_needed;++ci) {
        const uint64_t first=volume.first_data_sector+
            static_cast<uint64_t>(cluster-2u)*volume.sectors_per_cluster;
        for (uint32_t si=0u;si<volume.sectors_per_cluster && remaining!=0u;++si) {
            const uint64_t local=first+si;
            const uint64_t absolute=volume.partition.first_lba+local;
            if (absolute>0xFFFFFFFFu) return false;
            const uint32_t amount=remaining<volume.bytes_per_sector?remaining:volume.bytes_per_sector;
            const void* sector_data=source+offset;
            if (amount<volume.bytes_per_sector) {
                if (!fat32_read_sector(volume,static_cast<uint32_t>(local),sector_scratch)) return false;
                copy_bytes(sector_scratch,source+offset,amount);
                sector_data=sector_scratch;
            }
            if (!block_write(volume.device,static_cast<uint32_t>(absolute),1u,
                             sector_data,volume.bytes_per_sector)) return false;
            offset+=amount; remaining-=amount;
        }
        if (remaining!=0u) {
            uint32_t next=0u;
            if (!fat32_next_cluster(volume,cluster,&next)||next>=0x0FFFFFF8u) return false;
            cluster=next;
        }
    }
    if (remaining!=0u) return false;
    return block_flush(volume.device);
}

void storage_init() {
    registered_device_count = 0u;
    mounted_volume_count = 0u;
    mounted_rxfs_volume_count = 0u;

    const auto register_and_scan = [](const BlockDevice& candidate,
                                      const char* transport_name) {
        if (registered_device_count >= kMaxBlockDevices) return;
        block_devices[registered_device_count++] = candidate;
        starlix::drivers::serial::print("[STORAGE] ");
        starlix::drivers::serial::print(transport_name);
        starlix::drivers::serial::print(" block device ");
        print_dec(registered_device_count);
        starlix::drivers::serial::print(" capacity ");
        print_dec((candidate.last_lba + 1u) * candidate.block_size);
        starlix::drivers::serial::print(" bytes.\n");

        size_t partition_count = 0u;
        if (!partition_scan(candidate, partition_scratch,
                            kMaxPartitionsPerDevice, &partition_count)) {
            starlix::drivers::serial::print(
                "[STORAGE] PARTITION_SCAN=FAIL; device remains unmounted.\n");
            return;
        }
        starlix::drivers::serial::print(
            "[STORAGE] READ_ONLY_BLOCK_DEVICE=READY; sector zero and partition scan validated.\n");
        if (partition_count != 0u) {
            starlix::drivers::serial::print("[STORAGE] Valid partitions found: ");
            print_dec(partition_count);
            starlix::drivers::serial::print("\n");
            try_mount_partitions(candidate, partition_scratch, partition_count);
            return;
        }

        /* Also support whole-disk RXFS and FAT32 images without MBR/GPT. */
        Partition whole{};
        whole.present = true;
        whole.first_lba = 0u;
        whole.sector_count = candidate.last_lba + 1u;
        RxfsVolume rxfs_volume{};
        if (mounted_rxfs_volume_count < kMaxRxfsVolumes &&
            rxfs_try_mount(candidate, whole, &rxfs_volume)) {
            mounted_rxfs_volumes[mounted_rxfs_volume_count++] = rxfs_volume;
            starlix::drivers::serial::print("[RXFS] Mounted whole-disk read-only RXFS volume.\n");
        }
        Fat32Volume fat_volume{};
        if (fat32_try_mount(candidate, whole, &fat_volume) &&
            mounted_volume_count < kMaxBlockDevices) {
            mounted_volumes[mounted_volume_count++] = fat_volume;
            starlix::drivers::serial::print("[FAT32] Mounted whole-disk FAT32 volume (superfloppy).\n");
            log_volume(fat_volume);
            (void)fat32_list_root(fat_volume);
            smoke_test_mounted_volume(fat_volume);
        }
    };

    starlix::drivers::serial::print("[STORAGE] ATA PCI IDE discovery begin.\n");
    (void)starlix::drivers::ata::ata_pio_init();
    for (size_t index = 0u; index < starlix::drivers::ata::ata_pio_device_count(); ++index) {
        starlix::drivers::ata::AtaPioDeviceInfo info{};
        if (!starlix::drivers::ata::ata_pio_device_get(index, &info)) continue;
        BlockDevice device{};
        device.present = info.present;
        device.backend = BlockBackend::AtaPio;
        device.ata_channel = info.channel;
        device.ata_drive = info.drive;
        device.ata_command_base = info.command_base;
        device.ata_control_port = info.control_port;
        device.ata_lba48 = info.lba48;
        device.last_lba = info.last_lba;
        device.block_size = info.logical_sector_bytes;
        starlix::drivers::serial::print("[STORAGE] ATA IDENTIFY=PASS; PCI BDF ");
        print_dec(info.controller_bus); starlix::drivers::serial::print(":");
        print_dec(info.controller_slot); starlix::drivers::serial::print(".");
        print_dec(info.controller_function); starlix::drivers::serial::print(" channel ");
        print_dec(info.channel); starlix::drivers::serial::print(" drive ");
        print_dec(info.drive); starlix::drivers::serial::print(info.lba48
            ? " LBA48\n" : " LBA28\n");
        register_and_scan(device, "ATA PIO");
    }

    /* USB MSC discovery is optional and remains available when EHCI is ready. */
    for (uint8_t address = 1u; address < 128u; ++address) {
        starlix::drivers::ehci::MSCDeviceInfo info{};
        if (!starlix::drivers::ehci::ehci_msc_get_device_info(address, &info)) continue;
        if (registered_device_count >= kMaxBlockDevices) break;
        BlockDevice device{};
        device.present = true;
        device.backend = BlockBackend::UsbMassStorage;
        device.ehci_address = address;
        device.last_lba = info.last_lba;
        device.block_size = info.block_size;
        register_and_scan(device, "USB MSC");
    }
    starlix::drivers::serial::print("[STORAGE] Discovery complete; registered devices: ");
    print_dec(registered_device_count);
    starlix::drivers::serial::print("\n");
}

} // namespace starlix::storage
