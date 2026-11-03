#ifdef STBOOT_PHASE12_LARGE_FIXTURE
#include "stbootmngr_phase12_apfs_fixture.hpp"
#else
#include "stbootmngr_apfs_file_fixture.hpp"
#endif
#include "boot/x64/stbootmngr.efi/src/base_system_reader.hpp"
#include "boot/x64/stbootmngr.efi/src/stboot_handoff.hpp"

namespace {
#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t E2E_RXFS_BYTES = 16ULL * 1024ULL * 1024ULL;
#else
constexpr uint64_t E2E_RXFS_BYTES = 4ULL * 1024ULL * 1024ULL;
#endif
#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t E2E_DMG_CAPACITY = 9ULL * 1024ULL * 1024ULL;
#else
#ifdef STBOOT_PHASE12_LARGE_FIXTURE
constexpr uint64_t E2E_DMG_CAPACITY = 5ULL * 1024ULL * 1024ULL;
#else
constexpr uint64_t E2E_DMG_CAPACITY = 128ULL * 1024ULL;
#endif
#endif
static uint8_t E2eRxfsImage[E2E_RXFS_BYTES];
static uint8_t E2eDmgImage[E2E_DMG_CAPACITY];
/* Optional real production component embedded in the Phase 12 RXFS fixture. */
static uint8_t E2eStlcsImage[16384];
static uint64_t E2eStlcsSize = 0;
static uint8_t E2eMish[284];
static char E2eBase64Data[1024];
static char E2eXml[4096];
static uint64_t E2eDmgSize = 0;

static void E2eZero(void* dst, uint64_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; ++i) p[i] = 0;
}
static void E2eCopy(void* dst, const void* src, uint64_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}
static void E2ePut32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void E2ePut64(uint8_t* p, uint64_t v) {
    E2ePut32(p, (uint32_t)v); E2ePut32(p + 4, (uint32_t)(v >> 32));
}
static void E2eBe32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void E2eBe64(uint8_t* p, uint64_t v) {
    E2eBe32(p, (uint32_t)(v >> 32)); E2eBe32(p + 4, (uint32_t)v);
}
static uint32_t E2eCrc32c(const uint8_t* data, uint64_t n) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint64_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
    }
    return ~crc;
}
static void E2eAddText(char* out, uint64_t* pos, const char* text) {
    for (uint64_t i = 0; text[i]; ++i) out[(*pos)++] = text[i];
    out[*pos] = 0;
}
static uint64_t E2eEncodeBase64(const uint8_t* in, uint64_t n, char* out, uint64_t cap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint64_t pos = 0;
    for (uint64_t i = 0; i < n; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < n) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < n) ? in[i + 2] : 0;
        uint32_t x = (a << 16) | (b << 8) | c;
        if (pos + 4 >= cap) return 0;
        out[pos++] = t[(x >> 18) & 63];
        out[pos++] = t[(x >> 12) & 63];
        out[pos++] = (i + 1 < n) ? t[(x >> 6) & 63] : '=';
        out[pos++] = (i + 2 < n) ? t[x & 63] : '=';
    }
    if (pos < cap) out[pos] = 0;
    return pos;
}
static void E2eBuildDmg(bool compressed = false) {
    E2eZero(E2eDmgImage, sizeof(E2eDmgImage));
    E2eZero(E2eMish, sizeof(E2eMish));
    E2eZero(E2eBase64Data, sizeof(E2eBase64Data));
    E2eZero(E2eXml, sizeof(E2eXml));
    E2eCopy(E2eDmgImage, Image, sizeof(Image));

    // Minimal UDIF blkx/mish table exposing the full synthetic APFS container.
    E2eCopy(E2eMish, "mish", 4);
    E2eBe32(E2eMish + 4, 1);
    E2eBe64(E2eMish + 8, 0); // partition starts at virtual sector 0
    E2eBe64(E2eMish + 16, sizeof(Image) / 512);
    E2eBe64(E2eMish + 24, 0); // data-fork-relative offset
    E2eBe32(E2eMish + 200, 2); // stored run plus end marker
    E2eBe32(E2eMish + 204, compressed ? 0x80000005U : 1U);
    E2eBe64(E2eMish + 204 + 8, 0);
    E2eBe64(E2eMish + 204 + 16, sizeof(Image) / 512);
    E2eBe64(E2eMish + 204 + 24, 0);
    E2eBe64(E2eMish + 204 + 32, sizeof(Image));
    E2eBe32(E2eMish + 244, 0xFFFFFFFFU);

    if (!E2eEncodeBase64(E2eMish, sizeof(E2eMish), E2eBase64Data, sizeof(E2eBase64Data))) return;
    uint64_t xmlPos = 0;
    E2eAddText(E2eXml, &xmlPos,
        "<?xml version=\"1.0\"?><plist><dict><key>resource-fork</key><dict>"
        "<key>blkx</key><array><dict><key>Name</key><string>disk image (Apple_APFS : 3)"
        "</string><key>Data</key><data>");
    E2eAddText(E2eXml, &xmlPos, E2eBase64Data);
    E2eAddText(E2eXml, &xmlPos, "</data></dict></array></dict></dict></plist>");

    const uint64_t xmlOffset = sizeof(Image);
    E2eCopy(E2eDmgImage + xmlOffset, E2eXml, xmlPos);
    E2eDmgSize = xmlOffset + xmlPos + 512;
    uint8_t* footer = E2eDmgImage + E2eDmgSize - 512;
    E2eCopy(footer, "koly", 4);
    E2eBe32(footer + 4, 4);
    E2eBe32(footer + 8, 512);
    E2eBe64(footer + 24, 0);
    E2eBe64(footer + 32, sizeof(Image));
    E2eBe64(footer + 216, xmlOffset);
    E2eBe64(footer + 224, xmlPos);
    E2eBe64(footer + 492, sizeof(Image) / 512);
}
static void E2eAddDirEntry(uint8_t* block, uint64_t offset, uint64_t inode,
                           uint16_t entryLength, const char* name, uint16_t nameLength,
                           uint8_t fileType) {
    using namespace starlix::rxfs;
    auto* entry = reinterpret_cast<RXFS_DIR_ENTRY*>(block + offset);
    E2eZero(block + offset, entryLength);
    entry->inode_number = inode;
    entry->entry_length = entryLength;
    entry->name_length = nameLength;
    entry->file_type = fileType;
    for (uint16_t i = 0; i < nameLength; ++i) entry->name[i] = name[i];
    entry->name[nameLength] = '\0';
}
static void E2eSealInode(starlix::rxfs::RXFS_INODE* inode) {
    inode->inode_checksum = 0;
    inode->inode_checksum = E2eCrc32c(reinterpret_cast<const uint8_t*>(inode),
                                      sizeof(*inode) - sizeof(uint32_t));
}
static void E2eBuildRxfs() {
    using namespace starlix::rxfs;
    E2eZero(E2eRxfsImage, sizeof(E2eRxfsImage));
    RXFS_BOOT_SECTOR boot{};
    boot.jump_boot[0] = 0xEB; boot.jump_boot[1] = 0x58; boot.jump_boot[2] = 0x90;
    E2eCopy(boot.oem_name, RXFS_MAGIC, 8);
    boot.sector_size = 512; boot.sectors_per_cluster = 8; boot.reserved_sectors = 8;
    boot.total_sectors = sizeof(E2eRxfsImage) / 512;
    boot.superblock_lba = 1; boot.backup_sb_lba = (sizeof(E2eRxfsImage) / 512) - 1;
    boot.boot_signature = 0xAA55;
    E2eCopy(E2eRxfsImage, &boot, sizeof(boot));

    RXFS_SUPERBLOCK sb{};
    E2eCopy(sb.magic, RXFS_MAGIC, 8);
    sb.version_major = 1; sb.version_minor = 0;
    sb.total_blocks = sizeof(E2eRxfsImage) / 4096; sb.block_size = 4096;
    sb.total_inodes = 16; sb.free_blocks = 100;
    sb.free_inodes = E2eStlcsSize != 0u ? 11u : 13u;
    sb.max_volume_size = RXFS_MAX_VOLUME_SIZE; sb.max_file_size = RXFS_MAX_FILE_SIZE;
    sb.max_filename_len = RXFS_MAX_FILENAME_LEN;
    sb.inode_bitmap_lba = 1; sb.block_bitmap_lba = 2; sb.journal_lba = 3;
    sb.journal_size_blocks = 1; sb.inode_table_lba = 2; sb.data_blocks_lba = 4;
    sb.root_inode = 1; sb.features_enabled = RXFS_FEATURES_ALL;
    sb.superblock_checksum = 0;
    sb.superblock_checksum = E2eCrc32c(reinterpret_cast<const uint8_t*>(&sb), sizeof(sb) - sizeof(uint32_t));
    E2eCopy(E2eRxfsImage + 4096, &sb, sizeof(sb));

    const uint64_t dmgBlocks = (E2eDmgSize + 4095) / 4096;
    RXFS_INODE root{};
    root.inode_number = 1; root.file_type = RXFS_TYPE_DIRECTORY;
    root.permissions = 0755; root.link_count = 2;
    root.file_size = 24; root.allocated_blocks = 1; root.direct_blocks[0] = 4;
    E2eSealInode(&root);

    RXFS_INODE sys{};
    sys.inode_number = 2; sys.file_type = RXFS_TYPE_DIRECTORY;
    sys.permissions = 0755; sys.link_count = E2eStlcsSize != 0u ? 3u : 2u;
    sys.file_size = E2eStlcsSize != 0u ? 56u : 32u;
    sys.allocated_blocks = 1; sys.direct_blocks[0] = 5;
    E2eSealInode(&sys);

    RXFS_INODE base{};
    base.inode_number = 3; base.file_type = RXFS_TYPE_REGULAR;
    base.permissions = 0644; base.link_count = 1;
    base.file_size = E2eDmgSize; base.allocated_blocks = dmgBlocks;
    for (uint64_t i = 0; i < 12 && i < dmgBlocks; ++i) base.direct_blocks[i] = 6 + i;
    if (dmgBlocks > 12) base.indirect_block = 18;
    E2eSealInode(&base);

    E2eCopy(E2eRxfsImage + 2 * 4096, &root, sizeof(root));
    E2eCopy(E2eRxfsImage + 2 * 4096 + sizeof(root), &sys, sizeof(sys));
    E2eCopy(E2eRxfsImage + 2 * 4096 + 2 * sizeof(root), &base, sizeof(base));
    E2eAddDirEntry(E2eRxfsImage + 4 * 4096, 0, 2, 24, "sys", 3, RXFS_TYPE_DIRECTORY);
    E2eAddDirEntry(E2eRxfsImage + 5 * 4096, 0, 3, 32, "BaseSystem.dmg", 14, RXFS_TYPE_REGULAR);
    if (E2eStlcsSize != 0u) {
        if (E2eStlcsSize > sizeof(E2eStlcsImage) || E2eStlcsSize < 152u ||
            E2eStlcsImage[0] != 'P' || E2eStlcsImage[1] != 'x' ||
            sb.total_blocks < 16u) return;
        E2eAddDirEntry(E2eRxfsImage + 5 * 4096, 32, 4, 24,
                       "User", 4, RXFS_TYPE_DIRECTORY);
    }

    // The larger graphical boot.efi plus production kernel ECT needs an
    // 8 MiB APFS fixture. Exercise multiple RXFS double-indirect leaf tables.
    constexpr uint64_t kSinglePointerBlock = 18;
    constexpr uint64_t kDoubleRootBlock = 19;
    constexpr uint64_t kDoubleLeafStartBlock = 20;
    constexpr uint64_t kDoubleLeafCount = 4;
    constexpr uint64_t kSingleDataStart = kDoubleLeafStartBlock + kDoubleLeafCount;
    constexpr uint64_t kDoubleDataStart = kSingleDataStart + 512;
    constexpr uint64_t kSingleLimit = 12 + 512;
    constexpr uint64_t kDoubleLimit = kSingleLimit + kDoubleLeafCount * 512;
    if (dmgBlocks > kDoubleLimit) return; // fixture is intentionally bounded
    if (dmgBlocks > 12) base.indirect_block = kSinglePointerBlock;
    if (dmgBlocks > kSingleLimit) {
        base.double_indirect = kDoubleRootBlock;
        const uint64_t doubleBlocks = dmgBlocks - kSingleLimit;
        const uint64_t leafCount = (doubleBlocks + 511) / 512;
        for (uint64_t leaf = 0; leaf < leafCount; ++leaf) {
            E2ePut64(E2eRxfsImage + kDoubleRootBlock * 4096 + leaf * sizeof(uint64_t),
                     kDoubleLeafStartBlock + leaf);
        }
    }
    E2eSealInode(&base);
    E2eCopy(E2eRxfsImage + 2 * 4096 + 2 * sizeof(root), &base, sizeof(base));

    uint64_t dmgCopied = 0;
    for (uint64_t logical = 0; logical < dmgBlocks; ++logical) {
        uint64_t physical = 0;
        if (logical < 12) {
            physical = 6 + logical;
        } else if (logical < kSingleLimit) {
            const uint64_t pointerIndex = logical - 12;
            physical = kSingleDataStart + pointerIndex;
            E2ePut64(E2eRxfsImage + kSinglePointerBlock * 4096 +
                         pointerIndex * sizeof(uint64_t), physical);
        } else {
            const uint64_t pointerIndex = logical - kSingleLimit;
            const uint64_t leafIndex = pointerIndex / 512;
            const uint64_t leafOffset = pointerIndex % 512;
            physical = kDoubleDataStart + pointerIndex;
            E2ePut64(E2eRxfsImage +
                         (kDoubleLeafStartBlock + leafIndex) * 4096 +
                         leafOffset * sizeof(uint64_t), physical);
        }
        const uint64_t take = (E2eDmgSize - dmgCopied > 4096) ? 4096 : E2eDmgSize - dmgCopied;
        E2eCopy(E2eRxfsImage + physical * 4096, E2eDmgImage + dmgCopied, take);
        dmgCopied += take;
    }

    if (E2eStlcsSize != 0u) {
        /* Reserve the final volume block for the backup superblock. */
        const uint64_t dataBlocks = (E2eStlcsSize + 4095u) / 4096u;
        if (dataBlocks == 0u || dataBlocks > 4u ||
            sb.total_blocks <= dataBlocks + 2u) return;
        const uint64_t userDirectoryBlock = sb.total_blocks - dataBlocks - 2u;
        const uint64_t stlcsDataBlock = userDirectoryBlock + 1u;
        if (userDirectoryBlock <= dmgBlocks + 9u) return;

        RXFS_INODE userDirectory{};
        userDirectory.inode_number = 4;
        userDirectory.file_type = RXFS_TYPE_DIRECTORY;
        userDirectory.permissions = 0755;
        userDirectory.link_count = 2;
        userDirectory.file_size = 32;
        userDirectory.allocated_blocks = 1;
        userDirectory.direct_blocks[0] = userDirectoryBlock;
        E2eSealInode(&userDirectory);

        RXFS_INODE stlcs{};
        stlcs.inode_number = 5;
        stlcs.file_type = RXFS_TYPE_REGULAR;
        stlcs.permissions = 0444;
        stlcs.link_count = 1;
        stlcs.file_size = E2eStlcsSize;
        stlcs.allocated_blocks = dataBlocks;
        for (uint64_t block = 0u; block < dataBlocks; ++block)
            stlcs.direct_blocks[block] = stlcsDataBlock + block;
        E2eSealInode(&stlcs);

        E2eCopy(E2eRxfsImage + 2 * 4096 + 3 * sizeof(root),
                &userDirectory, sizeof(userDirectory));
        E2eCopy(E2eRxfsImage + 2 * 4096 + 4 * sizeof(root),
                &stlcs, sizeof(stlcs));
        E2eAddDirEntry(E2eRxfsImage + userDirectoryBlock * 4096,
                       0, 5, 32, "stlcs.ect", 9, RXFS_TYPE_REGULAR);
        uint64_t stlcsCopied = 0u;
        for (uint64_t block = 0u; block < dataBlocks; ++block) {
            const uint64_t take = E2eStlcsSize - stlcsCopied > 4096u
                ? 4096u : E2eStlcsSize - stlcsCopied;
            E2eCopy(E2eRxfsImage + (stlcsDataBlock + block) * 4096u,
                    E2eStlcsImage + stlcsCopied, take);
            stlcsCopied += take;
        }
    }
}
static EFI_STATUS E2eReadRxfs(void* context, uint64_t offset, UINTN length, void* buffer) {
    auto* data = static_cast<uint8_t*>(context);
    if (!data || !buffer || offset > sizeof(E2eRxfsImage) ||
        (uint64_t)length > sizeof(E2eRxfsImage) - offset) return EFI_LOAD_ERROR;
    E2eCopy(buffer, data + offset, length);
    return EFI_SUCCESS;
}
} // namespace

#ifndef STBOOT_PHASE11_EXPORT_ONLY
int main() {
    BuildFileImage();
    E2eBuildDmg(false);
    if (E2eDmgSize <= sizeof(Image) || E2eDmgSize > sizeof(E2eDmgImage)) return 1;
    E2eBuildRxfs();

    RXFS_SOURCE source{E2eReadRxfs, E2eRxfsImage, sizeof(E2eRxfsImage)};
    static RXFS_CONTEXT rxfs{};
    if (RxfsMountSource(&source, &rxfs) != EFI_SUCCESS) return 2;

    static UDIF_CONTEXT udif{}; // large run table must not live on the EFI stack
    static STBOOT_BASESYSTEM_CONTEXT system{};
    if (StbootOpenBaseSystem(&rxfs, 0, &udif, &system) != EFI_SUCCESS) return 3;
    if (!system.Opened || system.BootEfiPath.FileId != 13 || system.BootEfiPath.IsDirectory) return 4;

    STBOOT_FS_HANDOFF handoff{};
    if (StbootCreateFsHandoff(&system, &handoff) != EFI_SUCCESS) return 12;
    const uint8_t expectedSignature[8] = {'S','T','B','F','S','0','0','1'};
    for (uint32_t i = 0; i < 8; ++i)
        if (handoff.Signature[i] != expectedSignature[i]) return 13;
    if (handoff.Version != STBOOT_FS_HANDOFF_VERSION ||
        handoff.Size != sizeof(STBOOT_FS_HANDOFF) || !handoff.Context ||
        !handoff.Lookup || !handoff.GetInfo || !handoff.Read ||
        !(handoff.Flags & STBOOT_FS_HANDOFF_READ_ONLY) ||
        !(handoff.Flags & STBOOT_FS_HANDOFF_BOOT_SERVICES_REQUIRED)) return 14;

    uint64_t handoffFileId = 0;
    uint16_t handoffDirFlags = 0;
    BOOLEAN handoffIsDirectory = TRUE;
    if (handoff.Lookup(handoff.Context, "/System/Library/CoreServices/boot.efi",
                       &handoffFileId, &handoffDirFlags, &handoffIsDirectory) != EFI_SUCCESS ||
        handoffFileId != 13 || handoffIsDirectory != FALSE) return 15;

    STBOOT_FS_FILE_INFO handoffInfo{};
    if (handoff.GetInfo(handoff.Context, handoffFileId, &handoffInfo) != EFI_SUCCESS ||
        handoffInfo.FileId != 13 || handoffInfo.Size != 4100 ||
        (handoffInfo.Mode & 0xF000U) != 0x8000U) return 16;

    uint8_t handoffOut[16]{};
    UINTN handoffRead = 0;
    if (handoff.Read(handoff.Context, handoffFileId, 4096, sizeof(handoffOut),
                     handoffOut, &handoffRead) != EFI_SUCCESS || handoffRead != 4 ||
        handoffOut[0] != 'T' || handoffOut[1] != 'A' ||
        handoffOut[2] != 'I' || handoffOut[3] != 'L') return 17;

    APFS_FILE_INFO info{};
    if (StbootGetBootEfiInfo(&system, &info) != EFI_SUCCESS ||
        info.FileId != 13 || info.Size != 4100 || (info.Mode & 0xF000U) != 0x8000U) return 5;

    uint8_t out[16]{};
    UINTN bytesRead = 0;
    if (StbootReadBootEfi(&system, 4096, sizeof(out), out, &bytesRead) != EFI_SUCCESS || bytesRead != 4) return 6;
    if (out[0] != 'T' || out[1] != 'A' || out[2] != 'I' || out[3] != 'L') return 7;

    bytesRead = 99;
    if (StbootReadBootEfi(&system, 4100, sizeof(out), out, &bytesRead) != EFI_SUCCESS || bytesRead != 0) return 8;
    if (StbootReadBootEfi(&system, 0, sizeof(out), out, nullptr) != EFI_INVALID_PARAMETER) return 9;

    // Compressed UDIF input must fail closed; no undecoded bytes can reach the caller.
    E2eBuildDmg(true);
    E2eBuildRxfs();
    rxfs = {};
    source = {E2eReadRxfs, E2eRxfsImage, sizeof(E2eRxfsImage)};
    if (RxfsMountSource(&source, &rxfs) != EFI_SUCCESS) return 10;
    if (StbootOpenBaseSystem(&rxfs, 0, &udif, &system) == EFI_SUCCESS || system.Opened) return 11;
    return 0;
}
#endif // STBOOT_PHASE11_EXPORT_ONLY
