#define main apfs_path_original_selftest_main
#include "stbootmngr_apfs_path_selftest.cpp"
#undef main
#include "boot/x64/stbootmngr.efi/src/apfs_file_reader.hpp"

namespace {
struct FileRecord {
    uint8_t Key[300]{};
    uint16_t KeySize = 0;
    uint8_t Value[256]{};
    uint16_t ValueSize = 0;
};

static void BuildFileFsRoot(bool sparseSecond=false, bool encryptedSecond=false) {
    uint8_t* b = Image + 6 * BS;
    FileRecord rec[7]{};

    // Parent/name DREC keys must precede the child's inode/extent keys.
    const uint64_t parents[4] = {2, 10, 11, 12};
    const uint64_t ids[4] = {10, 11, 12, 13};
    const char* names[4] = {"System", "Library", "CoreServices", "boot.efi"};
    for (uint32_t i = 0; i < 4; ++i) {
        rec[i].KeySize = MakeDrecKey(rec[i].Key, parents[i], names[i], true);
        rec[i].ValueSize = 18;
        Put64(rec[i].Value, ids[i]);
        Put64(rec[i].Value + 8, 1000 + i);
        Put16(rec[i].Value + 16, i == 3 ? 8 : 4);
    }

    // Inode record, with one standard 40-byte DSTREAM extended field.
    rec[4].KeySize = 8;
    Put64(rec[4].Key, (3ULL << 60) | 13);
    rec[4].ValueSize = 140;
    Put64(rec[4].Value + 8, 13);       // private_id / stream OID
    Put16(rec[4].Value + 80, 0x81A4); // regular file, mode 0644
    Put16(rec[4].Value + 92, 1);      // xfield count
    Put16(rec[4].Value + 94, 40);     // used xfield data bytes
    rec[4].Value[96] = 8;             // INO_EXT_TYPE_DSTREAM
    rec[4].Value[97] = 0x20;          // XF_SYSTEM_FIELD
    Put16(rec[4].Value + 98, 40);
    Put64(rec[4].Value + 100, 4100);  // logical file size
    Put64(rec[4].Value + 108, 8192);  // allocated size

    // Two FILE_EXTENT records for the 4100-byte data stream.
    rec[5].KeySize = 16;
    Put64(rec[5].Key, (8ULL << 60) | 13);
    Put64(rec[5].Key + 8, 0);
    rec[5].ValueSize = 24;
    Put64(rec[5].Value, BS);
    Put64(rec[5].Value + 8, 14);
    Put64(rec[5].Value + 16, 0);

    rec[6].KeySize = 16;
    Put64(rec[6].Key, (8ULL << 60) | 13);
    Put64(rec[6].Key + 8, BS);
    rec[6].ValueSize = 24;
    Put64(rec[6].Value, BS);
    Put64(rec[6].Value + 8, sparseSecond ? 0 : 15);
    Put64(rec[6].Value + 16, encryptedSecond ? 1 : 0);

    const uint32_t count = 7, tocStart = 56, tocLen = count * 8, keyStart = tocStart + tocLen;
    const uint32_t valueEnd = BS - 40;
    Put16(b + 32, 0x0002U | 0x0001U); // root + leaf
    Put16(b + 34, 0);
    Put32(b + 36, count);
    Put16(b + 40, 0);
    Put16(b + 42, tocLen);
    uint32_t keyPos = 0, valueOff = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entryOff = tocStart + i * 8;
        Put16(b + entryOff, (uint16_t)keyPos);
        Put16(b + entryOff + 2, rec[i].KeySize);
        for (uint16_t j = 0; j < rec[i].KeySize; ++j) b[keyStart + keyPos + j] = rec[i].Key[j];
        keyPos += rec[i].KeySize;
        valueOff += rec[i].ValueSize;
        Put16(b + entryOff + 4, (uint16_t)valueOff);
        Put16(b + entryOff + 6, rec[i].ValueSize);
        const uint32_t valueStart = valueEnd - valueOff;
        for (uint16_t j = 0; j < rec[i].ValueSize; ++j) b[valueStart + j] = rec[i].Value[j];
    }
    ObjectHeader(b, 55, 100, 0x40000002U, 0x0EU);
    Put16(b + 32, 0x0002U | 0x0001U);
    Put16(b + 34, 0);
    Put32(b + 36, count);
    Put16(b + 40, 0);
    Put16(b + 42, tocLen);
    Put32(b + BS - 40 + 4, BS);
    Put32(b + BS - 40 + 8, 16);
    Put32(b + BS - 40 + 12, 16);
    Seal(b);
}

static void BuildFileImage(bool sparseSecond=false, bool encryptedSecond=false) {
    BuildImage(false); // create clean NXSB/OMAP/APSB scaffolding, then replace the FS tree
    BuildFileFsRoot(sparseSecond, encryptedSecond);
    for (uint32_t i = 0; i < BS; ++i) Image[14 * BS + i] = (uint8_t)('A' + (i % 26));
    Image[15 * BS + 0] = 'T';
    Image[15 * BS + 1] = 'A';
    Image[15 * BS + 2] = 'I';
    Image[15 * BS + 3] = 'L';
}
} // namespace
