#define __STDC_HOSTED__ 0
#include "boot/x64/stbootmngr.efi/src/apfs_reader.hpp"

namespace {
constexpr uint64_t kImageBytes = 4ULL * 1024ULL * 1024ULL;
static uint8_t gImage[kImageBytes];

static void CopyBytes(void* Dst, const void* Src, uint64_t N) {
    auto* d = static_cast<uint8_t*>(Dst);
    auto* s = static_cast<const uint8_t*>(Src);
    for (uint64_t i = 0; i < N; ++i) d[i] = s[i];
}
static void ZeroBytes(void* Dst, uint64_t N) {
    auto* d = static_cast<uint8_t*>(Dst);
    for (uint64_t i = 0; i < N; ++i) d[i] = 0;
}
static uint32_t Crc32c(const uint8_t* Data, uint64_t N) {
    uint32_t C = 0xFFFFFFFFU;
    for (uint64_t i = 0; i < N; ++i) {
        C ^= Data[i];
        for (uint32_t b = 0; b < 8; ++b)
            C = (C >> 1) ^ (0x82F63B78U & (0U - (C & 1U)));
    }
    return ~C;
}
static void Put32(uint8_t* P, uint32_t V) {
    P[0] = (uint8_t)V; P[1] = (uint8_t)(V >> 8);
    P[2] = (uint8_t)(V >> 16); P[3] = (uint8_t)(V >> 24);
}
static void Put64(uint8_t* P, uint64_t V) {
    Put32(P, (uint32_t)V); Put32(P + 4, (uint32_t)(V >> 32));
}
static EFI_STATUS ReadMemory(void* Context, uint64_t Offset, UINTN Length, void* Buffer) {
    auto* Bytes = static_cast<uint8_t*>(Context);
    if (Offset > kImageBytes || (uint64_t)Length > kImageBytes - Offset)
        return EFI_LOAD_ERROR;
    CopyBytes(Buffer, Bytes + Offset, Length);
    return EFI_SUCCESS;
}
static void AddDirEntry(uint8_t* Block, uint64_t Offset, uint64_t Inode,
                        uint16_t EntryLength, const char* Name, uint16_t NameLength,
                        uint8_t FileType) {
    auto* Entry = reinterpret_cast<starlix::rxfs::RXFS_DIR_ENTRY*>(Block + Offset);
    ZeroBytes(Block + Offset, EntryLength);
    Entry->inode_number = Inode;
    Entry->entry_length = EntryLength;
    Entry->name_length = NameLength;
    Entry->file_type = FileType;
    for (uint16_t i = 0; i < NameLength; ++i) Entry->name[i] = Name[i];
    Entry->name[NameLength] = '\0';
}
template <typename T>
static void SealInode(T* Inode) {
    Inode->inode_checksum = 0;
    Inode->inode_checksum = Crc32c(reinterpret_cast<const uint8_t*>(Inode), sizeof(T) - sizeof(uint32_t));
}
static void BuildImage() {
    using namespace starlix::rxfs;
    ZeroBytes(gImage, sizeof(gImage));

    RXFS_BOOT_SECTOR Boot{};
    Boot.jump_boot[0] = 0xEB; Boot.jump_boot[1] = 0x58; Boot.jump_boot[2] = 0x90;
    CopyBytes(Boot.oem_name, RXFS_MAGIC, 8);
    Boot.sector_size = 512;
    Boot.sectors_per_cluster = 8;
    Boot.reserved_sectors = 8;
    Boot.total_sectors = kImageBytes / 512;
    Boot.superblock_lba = 1;
    Boot.backup_sb_lba = (kImageBytes / 512) - 1;
    Boot.boot_signature = 0xAA55;
    CopyBytes(gImage, &Boot, sizeof(Boot));

    RXFS_SUPERBLOCK Sb{};
    CopyBytes(Sb.magic, RXFS_MAGIC, 8);
    Sb.version_major = 1; Sb.version_minor = 0;
    Sb.total_blocks = kImageBytes / 4096;
    Sb.block_size = 4096;
    Sb.total_inodes = 16;
    Sb.free_blocks = 100;
    Sb.free_inodes = 13;
    Sb.max_volume_size = RXFS_MAX_VOLUME_SIZE;
    Sb.max_file_size = RXFS_MAX_FILE_SIZE;
    Sb.max_filename_len = RXFS_MAX_FILENAME_LEN;
    Sb.inode_bitmap_lba = 1;
    Sb.block_bitmap_lba = 2;
    Sb.journal_lba = 3;
    Sb.journal_size_blocks = 1;
    Sb.inode_table_lba = 2;
    Sb.data_blocks_lba = 4;
    Sb.root_inode = 1;
    Sb.features_enabled = RXFS_FEATURES_ALL;
    Sb.superblock_checksum = 0;
    Sb.superblock_checksum = Crc32c(reinterpret_cast<const uint8_t*>(&Sb), sizeof(Sb) - sizeof(uint32_t));
    CopyBytes(gImage + 4096, &Sb, sizeof(Sb));

    RXFS_INODE Root{};
    Root.inode_number = 1; Root.file_type = RXFS_TYPE_DIRECTORY;
    Root.permissions = 0755; Root.link_count = 2;
    Root.file_size = 24; Root.allocated_blocks = 1; Root.direct_blocks[0] = 4;
    SealInode(&Root);

    RXFS_INODE Sys{};
    Sys.inode_number = 2; Sys.file_type = RXFS_TYPE_DIRECTORY;
    Sys.permissions = 0755; Sys.link_count = 2;
    Sys.file_size = 32; Sys.allocated_blocks = 1; Sys.direct_blocks[0] = 5;
    SealInode(&Sys);

    RXFS_INODE Base{};
    Base.inode_number = 3; Base.file_type = RXFS_TYPE_REGULAR;
    Base.permissions = 0644; Base.link_count = 1;
    Base.file_size = 4 * 4096; Base.allocated_blocks = 4;
    Base.direct_blocks[0] = 6; Base.direct_blocks[1] = 7;
    Base.direct_blocks[2] = 8; Base.direct_blocks[3] = 9;
    SealInode(&Base);

    CopyBytes(gImage + 2 * 4096, &Root, sizeof(Root));
    CopyBytes(gImage + 2 * 4096 + sizeof(Root), &Sys, sizeof(Sys));
    CopyBytes(gImage + 2 * 4096 + 2 * sizeof(Root), &Base, sizeof(Base));
    AddDirEntry(gImage + 4 * 4096, 0, 2, 24, "sys", 3, RXFS_TYPE_DIRECTORY);
    AddDirEntry(gImage + 5 * 4096, 0, 3, 32, "BaseSystem.dmg", 14, RXFS_TYPE_REGULAR);

    // A minimal raw APFS NXSB header in a four-block file; this checks only the
    // geometry-probe contract, not APFS B-tree/path resolution.
    uint8_t* Apfs = gImage + 6 * 4096;
    CopyBytes(Apfs + 32, "NXSB", 4);
    Put32(Apfs + 36, 4096);
    Put64(Apfs + 40, 4);
    Put64(Apfs + 160, 7);
    Put32(Apfs + 180, 1);
    Put64(Apfs + 184, 2);
}
} // namespace

int main() {
    BuildImage();
    RXFS_SOURCE Source{};
    Source.ReadAt = ReadMemory;
    Source.Context = gImage;
    Source.Size = sizeof(gImage);

    RXFS_CONTEXT Fs{};
    if (RxfsMountSource(&Source, &Fs) != EFI_SUCCESS) return 1;
    RXFS_FILE Base{};
    if (RxfsOpen(&Fs, "/sys/BaseSystem.dmg", &Base) != EFI_SUCCESS) return 2;
    if (Base.Inode.file_size != 4 * 4096) return 3;

    uint8_t Signature[4]{};
    if (RxfsReadFileRange(&Fs, &Base, 32, sizeof(Signature), Signature) != EFI_SUCCESS) return 4;
    if (Signature[0] != 'N' || Signature[1] != 'X' || Signature[2] != 'S' || Signature[3] != 'B') return 5;

    APFS_CONTAINER_INFO Info{};
    if (ApfsInspectRxfsFile(&Fs, &Base, &Info) != EFI_SUCCESS) return 6;
    if (Info.Kind != APFS_IMAGE_RAW_CONTAINER || !Info.HasValidGeometry ||
        Info.BlockSize != 4096 || Info.BlockCount != 4 ||
        Info.VolumeCount != 1 || Info.VolumeOids[0] != 2) return 7;

    // Reject a corrupt RXFS superblock checksum.
    RXFS_CONTEXT FsCorrupt{};
    gImage[4096 + 32] ^= 1;
    const EFI_STATUS CorruptStatus = RxfsMountSource(&Source, &FsCorrupt);
    gImage[4096 + 32] ^= 1;
    if (CorruptStatus == EFI_SUCCESS) return 8;

    // Reject impossible APFS geometry before attempting block access.
    uint8_t* Apfs = gImage + 6 * 4096;
    const uint8_t SavedCount[8] = {Apfs[40], Apfs[41], Apfs[42], Apfs[43],
                                   Apfs[44], Apfs[45], Apfs[46], Apfs[47]};
    Put64(Apfs + 40, 100);
    if (ApfsInspectRxfsFile(&Fs, &Base, &Info) == EFI_SUCCESS) return 9;
    CopyBytes(Apfs + 40, SavedCount, sizeof(SavedCount));

    // Recognize UDIF metadata while explicitly marking its block map as needed.
    Apfs[32] = 'X';
    CopyBytes(Apfs + 4 * 4096 - 512, "koly", 4);
    if (ApfsInspectRxfsFile(&Fs, &Base, &Info) != EFI_SUCCESS ||
        Info.Kind != APFS_IMAGE_UDIF_DMG || !Info.RequiresUdifMapping ||
        Info.HasValidGeometry) return 10;
    return 0;
}
