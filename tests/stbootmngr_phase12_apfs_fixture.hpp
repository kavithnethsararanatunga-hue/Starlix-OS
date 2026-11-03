#pragma once

// Used only by the Phase 12 host-side runtime-fixture exporter.
#define main phase12_apfs_path_original_selftest_main
#include "tests/stbootmngr_apfs_path_selftest.cpp"
#undef main
#include "boot/x64/stbootmngr.efi/src/apfs_file_reader.hpp"

namespace {
struct Phase12FileRecord {
    uint8_t Key[300]{};
    uint16_t KeySize = 0;
    uint8_t Value[256]{};
    uint16_t ValueSize = 0;
};

// STARLIX_PHASE13B_SYNTHETIC_SYS_FIXTURE
// Test SYS package is supplied by the host exporter and is never executed.
static const char kPhase13TestInfoPlist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<plist version=\"1.0\"><dict>"
    "<key>CFBundleIdentifier</key><string>com.starlix.test.example</string>"
    "<key>CFBundleExecutable</key><string>DB</string>"
    "<key>StarlixExecutable</key><string>ExampleKext.sys</string>"
    "</dict></plist>";

static void Phase12Zero(void* dst, uint64_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; ++i) p[i] = 0;
}
static void Phase12Copy(void* dst, const void* src, uint64_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}

static bool Phase12AddDrec(
    Phase12FileRecord* records, uint32_t capacity, uint32_t* count,
    uint64_t parent, uint64_t fileId, const char* name,
    uint16_t flags, bool hashed) {
    if (!records || !count || *count >= capacity) return false;
    Phase12FileRecord& r = records[(*count)++];
    r.KeySize = MakeDrecKey(r.Key, parent, name, hashed);
    r.ValueSize = 18;
    Put64(r.Value, fileId);
    Put64(r.Value + 8, 1000U + *count);
    Put16(r.Value + 16, flags);
    return r.KeySize != 0;
}

static bool Phase12AddInode(
    Phase12FileRecord* records, uint32_t capacity, uint32_t* count,
    uint64_t fileId, uint64_t streamId, uint64_t fileSize,
    uint64_t allocatedSize) {
    if (!records || !count || *count >= capacity) return false;
    Phase12FileRecord& r = records[(*count)++];
    r.KeySize = 8;
    Put64(r.Key, (3ULL << 60) | fileId);
    r.ValueSize = 140;
    Put64(r.Value + 8, streamId);
    Put16(r.Value + 80, 0x81A4); // regular file, mode 0644
    Put16(r.Value + 92, 1);      // one DSTREAM xfield
    Put16(r.Value + 94, 40);
    r.Value[96] = 8;             // INO_EXT_TYPE_DSTREAM
    r.Value[97] = 0x20;          // XF_SYSTEM_FIELD
    Put16(r.Value + 98, 40);
    Put64(r.Value + 100, fileSize);
    Put64(r.Value + 108, allocatedSize);
    return true;
}

static bool Phase12AddExtent(
    Phase12FileRecord* records, uint32_t capacity, uint32_t* count,
    uint64_t streamId, uint64_t logicalStart, uint64_t length,
    uint64_t physicalBlock) {
    if (!records || !count || *count >= capacity) return false;
    Phase12FileRecord& r = records[(*count)++];
    r.KeySize = 16;
    Put64(r.Key, (8ULL << 60) | streamId);
    Put64(r.Key + 8, logicalStart);
    r.ValueSize = 24;
    Put64(r.Value, length);
    Put64(r.Value + 8, physicalBlock);
    Put64(r.Value + 16, 0); // unencrypted test data
    return true;
}

static bool Phase12WriteFsTree(
    const Phase12FileRecord* records, uint32_t count) {
    if (!records || count == 0) return false;
    uint8_t* b = Image + 6 * BS;
    Phase12Zero(b, BS);

    const uint32_t tocStart = 56;
    const uint32_t tocLength = count * 8;
    const uint32_t keyStart = tocStart + tocLength;
    const uint32_t valueEnd = BS - 40;
    uint32_t keyPos = 0;
    uint32_t valueOff = 0;
    if (keyStart >= valueEnd) return false;

    for (uint32_t i = 0; i < count; ++i) {
        const Phase12FileRecord& r = records[i];
        if (r.KeySize == 0 || r.ValueSize == 0 ||
            keyStart + keyPos + r.KeySize > valueEnd) return false;
        valueOff += r.ValueSize;
        if (valueOff > valueEnd || valueOff > 0xFFFFU) return false;

        const uint32_t entryOff = tocStart + i * 8;
        Put16(b + entryOff, static_cast<uint16_t>(keyPos));
        Put16(b + entryOff + 2, r.KeySize);
        for (uint16_t j = 0; j < r.KeySize; ++j)
            b[keyStart + keyPos + j] = r.Key[j];
        keyPos += r.KeySize;

        Put16(b + entryOff + 4, static_cast<uint16_t>(valueOff));
        Put16(b + entryOff + 6, r.ValueSize);
        const uint32_t valueStart = valueEnd - valueOff;
        if (valueStart < keyStart + keyPos) return false;
        for (uint16_t j = 0; j < r.ValueSize; ++j)
            b[valueStart + j] = r.Value[j];
    }

    ObjectHeader(b, 55, 100, 0x40000002U, 0x0EU);
    Put16(b + 32, 0x0002U | 0x0001U); // root + leaf
    Put16(b + 34, 0);
    Put32(b + 36, count);
    Put16(b + 40, 0);
    Put16(b + 42, tocLength);
    Put32(b + BS - 40 + 4, BS);
    Put32(b + BS - 40 + 8, 16);
    Put32(b + BS - 40 + 12, 16);
    Seal(b);
    return true;
}

// The small ECT test kernel writes a serial marker and halts. It is not the
// production kernel. The boot.efi passed by the exporter is the real built image.
static bool Phase12ValidKextComponentName(
    const char* value, const char* requiredSuffix) {
    if (!value || !requiredSuffix) return false;
    uint32_t length = 0;
    uint32_t suffixLength = 0;
    while (length < 256U && value[length]) ++length;
    while (requiredSuffix[suffixLength]) ++suffixLength;
    if (length == 0 || length >= 256U || length <= suffixLength ||
        length > 255U) return false;
    for (uint32_t i = 0; i < length; ++i) {
        const char c = value[i];
        const bool allowed =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed || (i == 0 && c == '.')) return false;
        if (i != 0 && c == '.' && value[i - 1] == '.') return false;
    }
    for (uint32_t i = 0; i < suffixLength; ++i)
        if (value[length - suffixLength + i] != requiredSuffix[i]) return false;
    return true;
}

static bool BuildPhase12Image(
    const uint8_t* bootBytes, uint64_t bootSize,
    const uint8_t* ectBytes, uint64_t ectSize,
    const uint8_t* dbBytes, uint64_t dbSize,
    const uint8_t* infoPlistBytes, uint64_t infoPlistSize,
    const char* bundleName, const char* executableName) {
    if (!bootBytes || !bootSize || bootSize > 3ULL * 1024ULL * 1024ULL ||
        !ectBytes || !ectSize || ectSize > 4ULL * 1024ULL * 1024ULL ||
        !dbBytes || dbSize == 0 || dbSize > 1024ULL * 1024ULL ||
        !infoPlistBytes || infoPlistSize == 0 ||
        infoPlistSize > 64ULL * 1024ULL ||
        !Phase12ValidKextComponentName(bundleName, ".kext") ||
        !Phase12ValidKextComponentName(executableName, ".sys")) return false;

    BuildImage(false);
    Phase12Zero(Image + 6 * BS, BS);
    for (uint64_t i = 14; i < BLOCKS; ++i)
        Phase12Zero(Image + i * BS, BS);

    Phase12FileRecord rec[40]{};
    uint32_t count = 0;
    // APFS object-ID ordering: finish object 13 before adding higher-ID DRECs.
  // DREC entries sharing parent ID 2 must be ordered by name.
  if (!Phase12AddDrec(rec, 40, &count, 2, 20, "Starlix", 4, true) ||
      !Phase12AddDrec(rec, 40, &count, 2, 10, "System", 4, true) ||
      !Phase12AddDrec(rec, 40, &count, 10, 11, "Library", 4, false) ||
      !Phase12AddDrec(rec, 40, &count, 11, 12, "CoreServices", 4, true) ||
      !Phase12AddDrec(rec, 40, &count, 11, 22, "Extensions", 4, true) ||
      !Phase12AddDrec(rec, 40, &count, 11, 14, "Kernels", 4, true) ||
      !Phase12AddDrec(rec, 40, &count, 12, 13, "boot.efi", 8, true))
      return false;

  const uint64_t bootBlocks = (bootSize + BS - 1) / BS;
  const uint64_t kernelBlocks = (ectSize + BS - 1) / BS;
  const uint64_t infoBlocks = (infoPlistSize + BS - 1) / BS;
  const uint64_t dbBlocks = (dbSize + BS - 1) / BS;
  const uint64_t bootStartBlock = 14;
  const uint64_t kernelStartBlock = bootStartBlock + bootBlocks;
  const uint64_t kextInfoBlock = kernelStartBlock + kernelBlocks;
  const uint64_t kextDbBlock = kextInfoBlock + infoBlocks;
  if (bootBlocks == 0 || bootBlocks > 768 || kernelBlocks == 0 ||
      infoBlocks == 0 || dbBlocks == 0 ||
      kernelStartBlock + kernelBlocks > BLOCKS ||
      kextInfoBlock + infoBlocks > BLOCKS ||
      kextDbBlock > BLOCKS || dbBlocks > BLOCKS - kextDbBlock)
      return false;

  // Object ID 13: inode followed by its file extents.
  if (!Phase12AddInode(rec, 40, &count, 13, 13, bootSize, bootBlocks * BS) ||
      !Phase12AddExtent(rec, 40, &count, 13, 0, bootBlocks * BS,
                        bootStartBlock)) return false;

  // Object ID 15: /System/Library/Kernels/Kernel, stored as an ECT file.
  if (!Phase12AddDrec(rec, 40, &count, 14, 15, "Kernel", 8, true))
      return false;
  if (!Phase12AddInode(rec, 40, &count, 15, 15, ectSize, kernelBlocks * BS))
      return false;
  // The ECT occupies consecutive blocks, so a single extent represents it.
  if (!Phase12AddExtent(rec, 40, &count, 15, 0,
                         kernelBlocks * BS, kernelStartBlock))
      return false;

  // Preserve /Starlix/System as a compatibility directory; the ECT now lives
  // at /System/Library/Kernels/Kernel.
  if (!Phase12AddDrec(rec, 40, &count, 20, 21, "System", 4, true))
      return false;

  // KEXT bundle tree, in APFS object-ID/key order. The file extents are
  // contiguous and independently sized; Info.plist and SYS may span blocks.
  if (!Phase12AddDrec(rec, 40, &count, 22, 23, bundleName, 4, false) ||
      !Phase12AddDrec(rec, 40, &count, 23, 24, "Contents", 4, false) ||
      !Phase12AddDrec(rec, 40, &count, 24, 25, "Info.plist", 8, false) ||
      !Phase12AddDrec(rec, 40, &count, 24, 26, "Starlix", 4, false) ||
      !Phase12AddInode(rec, 40, &count, 25, 25,
                       infoPlistSize, infoBlocks * BS) ||
      !Phase12AddExtent(rec, 40, &count, 25, 0,
                        infoBlocks * BS, kextInfoBlock) ||
      !Phase12AddDrec(rec, 40, &count, 26, 27, executableName, 8, false) ||
      !Phase12AddInode(rec, 40, &count, 27, 27, dbSize, dbBlocks * BS) ||
      !Phase12AddExtent(rec, 40, &count, 27, 0,
                        dbBlocks * BS, kextDbBlock))
      return false;

  if (!Phase12WriteFsTree(rec, count)) return false;

    uint64_t copied = 0;
    for (uint64_t i = 0; i < bootBlocks; ++i) {
        const uint64_t take = (bootSize - copied > BS) ? BS : bootSize - copied;
        Phase12Copy(Image + (bootStartBlock + i) * BS, bootBytes + copied, take);
        copied += take;
    }
    copied = 0;
    for (uint64_t i = 0; i < kernelBlocks; ++i) {
        const uint64_t take = (ectSize - copied > BS) ? BS : ectSize - copied;
        Phase12Copy(Image + (kernelStartBlock + i) * BS, ectBytes + copied, take);
        copied += take;
    }
    Phase12Copy(Image + kextInfoBlock * BS, infoPlistBytes, infoPlistSize);
    Phase12Copy(Image + kextDbBlock * BS, dbBytes, dbSize);
    return true;
}
} // namespace
