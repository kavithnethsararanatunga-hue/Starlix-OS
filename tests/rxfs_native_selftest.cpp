#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "../kernel/src/rxfs_native.hpp"

using namespace starlix::rxfs;
using namespace starlix::kernel::rxfs;

namespace {
struct MemoryVolume {
    std::vector<uint8_t> bytes;
    uint64_t reads = 0;
    uint64_t rejected_reads = 0;
};

bool memory_read_block(void* opaque, uint64_t lba, void* output, uint32_t capacity) {
    auto* memory = static_cast<MemoryVolume*>(opaque);
    if (!memory || !output || capacity < RXFS_BLOCK_SIZE ||
        lba >= memory->bytes.size() / RXFS_BLOCK_SIZE) {
        if (memory) ++memory->rejected_reads;
        return false;
    }
    std::memcpy(output, memory->bytes.data() + lba * RXFS_BLOCK_SIZE,
                RXFS_BLOCK_SIZE);
    ++memory->reads;
    return true;
}

void write_block(MemoryVolume& memory, uint64_t lba, const void* data, size_t size) {
    std::memcpy(memory.bytes.data() + lba * RXFS_BLOCK_SIZE, data, size);
}

void seal_inode(RXFS_INODE& inode) {
    inode.inode_checksum = 0u;
    inode.inode_checksum = rxfs_crc32c(&inode, sizeof(inode) - sizeof(uint32_t));
}

void write_inode(MemoryVolume& memory, uint64_t inode_number, RXFS_INODE inode) {
    seal_inode(inode);
    std::memcpy(memory.bytes.data() + 5u * RXFS_BLOCK_SIZE +
                    (inode_number - 1u) * RXFS_INODE_SIZE,
                &inode, sizeof(inode));
}
size_t add_dir_entry(uint8_t* block, size_t offset, uint64_t inode_number,
                     uint8_t type, const char* name) {
    const size_t name_length = std::strlen(name);
    const size_t entry_length =
        (sizeof(RXFS_DIR_ENTRY) + name_length + 1u + 7u) & ~size_t(7u);
    auto* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(block + offset);
    entry->inode_number = inode_number;
    entry->entry_length = static_cast<uint16_t>(entry_length);
    entry->name_length = static_cast<uint16_t>(name_length);
    entry->file_type = type;
    std::memcpy(entry->name, name, name_length + 1u);
    return offset + entry_length;
}

MemoryVolume make_fixture() {
    constexpr uint64_t kBlocks = 64u;
    MemoryVolume memory{};
    memory.bytes.resize(kBlocks * RXFS_BLOCK_SIZE);

    RXFS_BOOT_SECTOR boot{};
    boot.jump_boot[0] = 0xEB;
    boot.jump_boot[1] = 0x58;
    boot.jump_boot[2] = 0x90;
    std::memcpy(boot.oem_name, RXFS_MAGIC, 8u);
    boot.sector_size = RXFS_SECTOR_SIZE;
    boot.sectors_per_cluster = 8u;
    boot.reserved_sectors = 8u;
    boot.total_sectors = kBlocks * (RXFS_BLOCK_SIZE / RXFS_SECTOR_SIZE);
    boot.superblock_lba = 1u;
    boot.backup_sb_lba = boot.total_sectors - 1u;
    boot.boot_signature = 0xAA55u;
    write_block(memory, 0u, &boot, sizeof(boot));

    RXFS_SUPERBLOCK superblock{};
    init_superblock(superblock, memory.bytes.size());
    superblock.total_blocks = kBlocks;
    superblock.total_inodes = 16u;
    superblock.inode_bitmap_lba = 2u;
    superblock.block_bitmap_lba = 3u;
    superblock.journal_lba = 4u;
    superblock.journal_size_blocks = 1u;
    superblock.inode_table_lba = 5u;
    superblock.data_blocks_lba = 7u;
    superblock.root_inode = 1u;
    superblock.superblock_checksum = 0u;
    superblock.superblock_checksum =
        rxfs_crc32c(&superblock, sizeof(superblock) - sizeof(uint32_t));
    write_block(memory, 1u, &superblock, sizeof(superblock));

    uint8_t root_block[RXFS_BLOCK_SIZE]{};
    size_t root_used = 0u;
    root_used = add_dir_entry(root_block, root_used, 1u, RXFS_TYPE_DIRECTORY, ".");
    root_used = add_dir_entry(root_block, root_used, 1u, RXFS_TYPE_DIRECTORY, "..");
    root_used = add_dir_entry(root_block, root_used, 2u, RXFS_TYPE_DIRECTORY, "sys");
    root_used = add_dir_entry(root_block, root_used, 8u, RXFS_TYPE_DIRECTORY, "Applications");
    write_block(memory, 7u, root_block, sizeof(root_block));

    uint8_t sys_block[RXFS_BLOCK_SIZE]{};
    size_t sys_used = 0u;
    sys_used = add_dir_entry(sys_block, sys_used, 2u, RXFS_TYPE_DIRECTORY, ".");
    sys_used = add_dir_entry(sys_block, sys_used, 1u, RXFS_TYPE_DIRECTORY, "..");
    sys_used = add_dir_entry(sys_block, sys_used, 3u, RXFS_TYPE_DIRECTORY, "User");
    write_block(memory, 8u, sys_block, sizeof(sys_block));

    uint8_t user_block[RXFS_BLOCK_SIZE]{};
    size_t user_used = 0u;
    user_used = add_dir_entry(user_block, user_used, 3u, RXFS_TYPE_DIRECTORY, ".");
    user_used = add_dir_entry(user_block, user_used, 2u, RXFS_TYPE_DIRECTORY, "..");
    user_used = add_dir_entry(user_block, user_used, 4u, RXFS_TYPE_REGULAR, "smgrsb.ect");
    user_used = add_dir_entry(user_block, user_used, 5u, RXFS_TYPE_REGULAR, "stitzn.ect");
    user_used = add_dir_entry(user_block, user_used, 6u, RXFS_TYPE_REGULAR, "lsass.ect");
    user_used = add_dir_entry(user_block, user_used, 7u, RXFS_TYPE_REGULAR, "stlgn.ect");
    write_block(memory, 9u, user_block, sizeof(user_block));

    RXFS_INODE root{};
    root.inode_number = 1u;
    root.file_type = RXFS_TYPE_DIRECTORY;
    root.permissions = 0755u;
    root.allocated_blocks = 1u;
    root.link_count = 2u;
    root.file_size = root_used;
    root.direct_blocks[0] = 7u;
    write_inode(memory, 1u, root);

    RXFS_INODE sys{};
    sys.inode_number = 2u;
    sys.file_type = RXFS_TYPE_DIRECTORY;
    sys.permissions = 0755u;
    sys.allocated_blocks = 1u;
    sys.link_count = 2u;
    sys.file_size = sys_used;
    sys.direct_blocks[0] = 8u;
    write_inode(memory, 2u, sys);

    RXFS_INODE user{};
    user.inode_number = 3u;
    user.file_type = RXFS_TYPE_DIRECTORY;
    user.permissions = 0700u;
    user.allocated_blocks = 1u;
    user.link_count = 2u;
    user.file_size = user_used;
    user.direct_blocks[0] = 9u;
    write_inode(memory, 3u, user);

    const char* names[4] = {"smgrsb.ect", "stitzn.ect", "lsass.ect", "stlgn.ect"};
    const uint64_t sizes[4] = {18u, 17u, 16u,
        12u * RXFS_BLOCK_SIZE + 100u};
    const uint64_t first_blocks[4] = {10u, 11u, 12u, 13u};
    for (uint64_t i = 0u; i < 4u; ++i) {
        RXFS_INODE file{};
        file.inode_number = 4u + i;
        file.file_type = RXFS_TYPE_REGULAR;
        file.permissions = 0600u;
        file.file_size = sizes[i];
        file.allocated_blocks = i == 3u ? 14u : 1u;
        file.link_count = 1u;
        file.direct_blocks[0] = first_blocks[i];
        if (i == 3u) {
            for (uint64_t direct = 0u; direct < 12u; ++direct)
                file.direct_blocks[direct] = 13u + direct;
            file.indirect_block = 25u;
        }
        write_inode(memory, 4u + i, file);

        if (i != 3u) {
            const char payload[] = "STARLIX-ECT-COMPONENT";
            write_block(memory, first_blocks[i], payload, sizes[i]);
        } else {
            std::vector<uint8_t> payload(static_cast<size_t>(sizes[i]));
            for (size_t n = 0; n < payload.size(); ++n)
                payload[n] = static_cast<uint8_t>(n & 0xFFu);
            for (uint64_t direct = 0u; direct < 12u; ++direct)
                write_block(memory, 13u + direct,
                            payload.data() + direct * RXFS_BLOCK_SIZE,
                            RXFS_BLOCK_SIZE);
            uint64_t indirect_entries[RXFS_BLOCK_SIZE / sizeof(uint64_t)]{};
            indirect_entries[0] = 26u;
            write_block(memory, 25u, indirect_entries, sizeof(indirect_entries));
            write_block(memory, 26u, payload.data() + 12u * RXFS_BLOCK_SIZE,
                        payload.size() - 12u * RXFS_BLOCK_SIZE);
        }
        (void)names[i];
    }

    uint8_t applications_block[RXFS_BLOCK_SIZE]{};
    size_t applications_used = 0u;
    applications_used = add_dir_entry(applications_block, applications_used, 8u,
                                      RXFS_TYPE_DIRECTORY, ".");
    applications_used = add_dir_entry(applications_block, applications_used, 1u,
                                      RXFS_TYPE_DIRECTORY, "..");
    applications_used = add_dir_entry(applications_block, applications_used, 9u,
                                      RXFS_TYPE_DIRECTORY, "Explorer.app");
    write_block(memory, 27u, applications_block, sizeof(applications_block));

    uint8_t explorer_block[RXFS_BLOCK_SIZE]{};
    size_t explorer_used = 0u;
    explorer_used = add_dir_entry(explorer_block, explorer_used, 9u,
                                  RXFS_TYPE_DIRECTORY, ".");
    explorer_used = add_dir_entry(explorer_block, explorer_used, 8u,
                                  RXFS_TYPE_DIRECTORY, "..");
    explorer_used = add_dir_entry(explorer_block, explorer_used, 10u,
                                  RXFS_TYPE_DIRECTORY, "Contents");
    write_block(memory, 28u, explorer_block, sizeof(explorer_block));

    uint8_t contents_block[RXFS_BLOCK_SIZE]{};
    size_t contents_used = 0u;
    contents_used = add_dir_entry(contents_block, contents_used, 10u,
                                  RXFS_TYPE_DIRECTORY, ".");
    contents_used = add_dir_entry(contents_block, contents_used, 9u,
                                  RXFS_TYPE_DIRECTORY, "..");
    contents_used = add_dir_entry(contents_block, contents_used, 11u,
                                  RXFS_TYPE_DIRECTORY, "Resources");
    contents_used = add_dir_entry(contents_block, contents_used, 12u,
                                  RXFS_TYPE_REGULAR, "config.plist");
    contents_used = add_dir_entry(contents_block, contents_used, 13u,
                                  RXFS_TYPE_REGULAR, "explorer.ect");
    write_block(memory, 29u, contents_block, sizeof(contents_block));

    uint8_t resources_block[RXFS_BLOCK_SIZE]{};
    size_t resources_used = 0u;
    resources_used = add_dir_entry(resources_block, resources_used, 11u,
                                   RXFS_TYPE_DIRECTORY, ".");
    resources_used = add_dir_entry(resources_block, resources_used, 10u,
                                   RXFS_TYPE_DIRECTORY, "..");
    resources_used = add_dir_entry(resources_block, resources_used, 14u,
                                   RXFS_TYPE_REGULAR, "icon.png");
    resources_used = add_dir_entry(resources_block, resources_used, 15u,
                                   RXFS_TYPE_REGULAR, "mc.bmp");
    write_block(memory, 30u, resources_block, sizeof(resources_block));

    const uint64_t directory_numbers[] = {8u, 9u, 10u, 11u};
    const uint64_t directory_blocks[] = {27u, 28u, 29u, 30u};
    const uint64_t directory_sizes[] = {applications_used, explorer_used,
                                        contents_used, resources_used};
    const uint64_t directory_permissions[] = {0755u, 0755u, 0755u, 0755u};
    for (size_t i = 0u; i < 4u; ++i) {
        RXFS_INODE directory{};
        directory.inode_number = directory_numbers[i];
        directory.file_type = RXFS_TYPE_DIRECTORY;
        directory.permissions = static_cast<uint16_t>(directory_permissions[i]);
        directory.allocated_blocks = 1u;
        directory.link_count = 2u;
        directory.file_size = directory_sizes[i];
        directory.direct_blocks[0] = directory_blocks[i];
        write_inode(memory, directory_numbers[i], directory);
    }

    const char* app_file_names[] = {"config.plist", "explorer.ect", "icon.png", "mc.bmp"};
    const char* app_payloads[] = {"<plist>STARLIX</plist>", "STARLIX-EXPLORER-ECT",
                                  "PNG-RESOURCE", "BMP-RESOURCE"};
    for (size_t i = 0u; i < 4u; ++i) {
        RXFS_INODE file{};
        file.inode_number = 12u + i;
        file.file_type = RXFS_TYPE_REGULAR;
        file.permissions = 0644u;
        file.file_size = std::strlen(app_payloads[i]);
        file.allocated_blocks = 1u;
        file.link_count = 1u;
        file.direct_blocks[0] = 31u + i;
        write_inode(memory, file.inode_number, file);
        write_block(memory, 31u + i, app_payloads[i],
                    static_cast<size_t>(file.file_size));
        (void)app_file_names[i];
    }
    return memory;
}

bool expect(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    return condition;
}
}

int main(int argc, char** argv) {
    bool ok = true;
    MemoryVolume memory = make_fixture();
    NativeRxfsContext context{};
    ok &= expect(MountNativeRxfs(&memory, memory_read_block,
                                 memory.bytes.size() / RXFS_BLOCK_SIZE, &context),
                 "mount validates RXFS boot sector and superblock checksum");

    NativeRxfsNode node{};
    ok &= expect(LookupNativeRxfsPath(&context,
                                      "RXFS\\sys\\User\\smgrsb.ect", &node) &&
                 node.inode_number == 4u && node.file_type == RXFS_TYPE_REGULAR,
                 "Windows-style RXFS path resolves user subsystem file");
    ok &= expect(LookupNativeRxfsPath(&context,
                                      "/sys/User/stlgn.ect", &node) &&
                 node.inode_number == 7u,
                 "POSIX-style nested path resolves login component");
    ok &= expect(LookupNativeRxfsPath(&context,
                                      "RXFS\\sys\\User\\stitzn.ect", &node) &&
                 node.inode_number == 5u,
                 "system initialization component path resolves");
    ok &= expect(LookupNativeRxfsPath(&context,
                                      "RXFS\\Applications\\Explorer.app\\Contents\\config.plist",
                                      &node) && node.inode_number == 12u,
                 "planned .app bundle config path resolves through RXFS");
    ok &= expect(LookupNativeRxfsPath(&context,
                                      "RXFS\\Applications\\Explorer.app\\Contents\\Resources\\icon.png",
                                      &node) && node.inode_number == 14u,
                 "planned .app resource path resolves through nested directories");

    uint8_t small_buffer[64]{};
    uint32_t bytes_read = 0u;
    ok &= expect(ReadNativeRxfsFile(&context, "RXFS\\sys\\User\\lsass.ect",
                                    small_buffer, sizeof(small_buffer), &bytes_read) &&
                 bytes_read == 16u &&
                 std::memcmp(small_buffer, "STARLIX-ECT-COMP", 16u) == 0,
                 "read-only file API returns exact component bytes");
    bytes_read = 0u;
    ok &= expect(ReadNativeRxfsFile(&context,
                                    "RXFS\\Applications\\Explorer.app\\Contents\\explorer.ect",
                                    small_buffer, sizeof(small_buffer), &bytes_read) &&
                 bytes_read == std::strlen("STARLIX-EXPLORER-ECT") &&
                 std::memcmp(small_buffer, "STARLIX-EXPLORER-ECT", bytes_read) == 0,
                 "application ECT can be read from the planned .app package path");

    std::vector<uint8_t> large_buffer(12u * RXFS_BLOCK_SIZE + 100u);
    bytes_read = 0u;
    ok &= expect(ReadNativeRxfsFile(&context, "sys/User/stlgn.ect",
                                    large_buffer.data(), large_buffer.size(), &bytes_read) &&
                 bytes_read == large_buffer.size() &&
                 large_buffer[0] == 0u && large_buffer[4096] == 0u &&
                 large_buffer[12u * RXFS_BLOCK_SIZE] ==
                    static_cast<uint8_t>((12u * RXFS_BLOCK_SIZE) & 0xFFu) &&
                 large_buffer.back() == static_cast<uint8_t>((large_buffer.size() - 1u) & 0xFFu),
                 "file reader resolves direct plus single-indirect data blocks");

    bytes_read = 99u;
    ok &= expect(!ReadNativeRxfsFile(&context, "sys/User/stlgn.ect",
                                     small_buffer, sizeof(small_buffer), &bytes_read) &&
                 bytes_read == 0u,
                 "undersized output buffer fails without reporting bytes");
    ok &= expect(!LookupNativeRxfsPath(&context,
                                       "sys/User/../stlgn.ect", &node),
                 "path traversal with parent component is rejected");
    ok &= expect(!ReadNativeRxfsFile(&context, "sys/User/missing.ect",
                                     small_buffer, sizeof(small_buffer), &bytes_read),
                 "missing path fails closed");
    ok &= expect(!ReadNativeRxfsFile(&context, "sys/User",
                                     small_buffer, sizeof(small_buffer), &bytes_read),
                 "directory cannot be read as a regular file");

    MemoryVolume bad_super = make_fixture();
    bad_super.bytes[RXFS_BLOCK_SIZE + 96u] ^= 0x40u;
    NativeRxfsContext bad_context{};
    ok &= expect(!MountNativeRxfs(&bad_super, memory_read_block,
                                  bad_super.bytes.size() / RXFS_BLOCK_SIZE,
                                  &bad_context),
                 "corrupt superblock is rejected by checksum validation");

    MemoryVolume mismatched_uuid = make_fixture();
    RXFS_BOOT_SECTOR mismatched_boot{};
    std::memcpy(&mismatched_boot, mismatched_uuid.bytes.data(), sizeof(mismatched_boot));
    mismatched_boot.volume_uuid[0] ^= 0x80u;
    write_block(mismatched_uuid, 0u, &mismatched_boot, sizeof(mismatched_boot));
    NativeRxfsContext uuid_context{};
    ok &= expect(!MountNativeRxfs(&mismatched_uuid, memory_read_block,
                                  mismatched_uuid.bytes.size() / RXFS_BLOCK_SIZE,
                                  &uuid_context),
                 "boot-sector and superblock volume UUID mismatch is rejected");

    MemoryVolume bad_backup_lba = make_fixture();
    RXFS_BOOT_SECTOR bad_backup_boot{};
    std::memcpy(&bad_backup_boot, bad_backup_lba.bytes.data(), sizeof(bad_backup_boot));
    bad_backup_boot.backup_sb_lba = bad_backup_boot.total_sectors;
    write_block(bad_backup_lba, 0u, &bad_backup_boot, sizeof(bad_backup_boot));
    NativeRxfsContext backup_context{};
    ok &= expect(!MountNativeRxfs(&bad_backup_lba, memory_read_block,
                                  bad_backup_lba.bytes.size() / RXFS_BLOCK_SIZE,
                                  &backup_context),
                 "out-of-volume backup superblock sector is rejected");

    MemoryVolume bad_inode = make_fixture();
    bad_inode.bytes[5u * RXFS_BLOCK_SIZE + 3u * RXFS_INODE_SIZE + 40u] ^= 0x01u;
    NativeRxfsContext inode_context{};
    bool mounted = MountNativeRxfs(&bad_inode, memory_read_block,
                                   bad_inode.bytes.size() / RXFS_BLOCK_SIZE,
                                   &inode_context);
    ok &= expect(mounted && !LookupNativeRxfsPath(&inode_context,
                                                  "sys/User/smgrsb.ect", &node),
                 "corrupt inode checksum prevents path resolution");

    MemoryVolume bad_data_pointer = make_fixture();
    const size_t inode4_offset = 5u * RXFS_BLOCK_SIZE + 3u * RXFS_INODE_SIZE;
    RXFS_INODE corrupt_file{};
    std::memcpy(&corrupt_file, bad_data_pointer.bytes.data() + inode4_offset,
                sizeof(corrupt_file));
    corrupt_file.direct_blocks[0] = 0xFFFFFFFFu;
    seal_inode(corrupt_file);
    std::memcpy(bad_data_pointer.bytes.data() + inode4_offset,
                &corrupt_file, sizeof(corrupt_file));
    NativeRxfsContext pointer_context{};
    mounted = MountNativeRxfs(&bad_data_pointer, memory_read_block,
                              bad_data_pointer.bytes.size() / RXFS_BLOCK_SIZE,
                              &pointer_context);
    bytes_read = 0u;
    ok &= expect(mounted && !ReadNativeRxfsFile(&pointer_context,
                                                "sys/User/smgrsb.ect",
                                                small_buffer, sizeof(small_buffer),
                                                &bytes_read),
                 "out-of-volume file block pointer is rejected");

    MemoryVolume malformed_dir = make_fixture();
    auto* malformed_entry = reinterpret_cast<RXFS_DIR_ENTRY*>(
        malformed_dir.bytes.data() + 8u * RXFS_BLOCK_SIZE + 48u);
    malformed_entry->entry_length = 0xFFF8u;
    NativeRxfsContext malformed_context{};
    mounted = MountNativeRxfs(&malformed_dir, memory_read_block,
                              malformed_dir.bytes.size() / RXFS_BLOCK_SIZE,
                              &malformed_context);
    ok &= expect(mounted && !LookupNativeRxfsPath(&malformed_context,
                                                  "sys/User/smgrsb.ect", &node),
                 "malformed directory record length is rejected");

    MemoryVolume compressed_file = make_fixture();
    std::memcpy(&corrupt_file, compressed_file.bytes.data() + inode4_offset,
                sizeof(corrupt_file));
    corrupt_file.flags |= INODE_FLAG_COMPRESSED;
    seal_inode(corrupt_file);
    std::memcpy(compressed_file.bytes.data() + inode4_offset,
                &corrupt_file, sizeof(corrupt_file));
    NativeRxfsContext compressed_context{};
    mounted = MountNativeRxfs(&compressed_file, memory_read_block,
                              compressed_file.bytes.size() / RXFS_BLOCK_SIZE,
                              &compressed_context);
    bytes_read = 0u;
    ok &= expect(mounted && !ReadNativeRxfsFile(&compressed_context,
                                                "sys/User/smgrsb.ect",
                                                small_buffer, sizeof(small_buffer),
                                                &bytes_read),
                 "unsupported compressed file is rejected rather than returned as raw bytes");

    if (argc > 1) {
        std::ifstream image_file(argv[1], std::ios::binary);
        std::vector<uint8_t> image_bytes(
            (std::istreambuf_iterator<char>(image_file)), std::istreambuf_iterator<char>());
        MemoryVolume disk_image{};
        disk_image.bytes = std::move(image_bytes);
        NativeRxfsContext disk_context{};
        const bool disk_mount = !image_file.bad() && !disk_image.bytes.empty() &&
            (disk_image.bytes.size() % RXFS_BLOCK_SIZE) == 0u &&
            MountNativeRxfs(&disk_image, memory_read_block,
                            disk_image.bytes.size() / RXFS_BLOCK_SIZE, &disk_context);
        ok &= expect(disk_mount,
                     "mount accepts an RXFS image produced by the project formatter");
        if (disk_mount) {
            uint8_t disk_file[512]{};
            uint32_t disk_bytes = 0u;
            const bool welcome_read = ReadNativeRxfsFile(
                &disk_context, "RXFS\\welcome.txt", disk_file,
                sizeof(disk_file), &disk_bytes);
            ok &= expect(welcome_read && disk_bytes != 0u &&
                         std::memcmp(disk_file, "Welcome to RXFS", 15u) == 0,
                         "formatter-generated root file can be read through native path API");
            disk_bytes = 0u;
            const bool sys_read = ReadNativeRxfsFile(
                &disk_context, "/starlix.sys", disk_file,
                sizeof(disk_file), &disk_bytes);
            ok &= expect(sys_read && disk_bytes != 0u &&
                         std::memcmp(disk_file, "STARLIX_OS_VERSION=1.0.Beta", 27u) == 0,
                         "formatter-generated metadata file can be read");
        }
    }

    std::printf("RXFS_NATIVE_SELFTEST: %s (reads=%llu, bounds_rejections=%llu)\n",
                ok ? "PASS" : "FAIL",
                static_cast<unsigned long long>(memory.reads),
                static_cast<unsigned long long>(memory.rejected_reads));
    return ok ? 0 : 1;
}
