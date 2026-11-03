#define STBOOT_PHASE12_LARGE_FIXTURE
#define STBOOT_PHASE11_EXPORT_ONLY
#include "tests/stbootmngr_basesystem_selftest.cpp"
#include "boot/x64/stbootmngr.efi/src/rxfs_reader.hpp"
#undef STBOOT_PHASE11_EXPORT_ONLY
#undef STBOOT_PHASE12_LARGE_FIXTURE

// Minimal Linux host I/O declarations avoid clashes with the project's
// freestanding aliases for size_t and fixed-width integer types.
extern "C" {
int open(const char* pathname, int flags, ...);
long read(int fd, void* buffer, unsigned long count);
long write(int fd, const void* buffer, unsigned long count);
long lseek(int fd, long offset, int whence);
int close(int fd);
}

static uint8_t gBootBuffer[4 * 1024 * 1024];
static uint8_t gEctBuffer[4 * 1024 * 1024];
static uint8_t gDbBuffer[1024 * 1024];
static uint8_t gInfoPlistBuffer[64 * 1024];

static bool WriteAll(int fd, const void* data, unsigned long long length) {
    const char* bytes = static_cast<const char*>(data);
    unsigned long long done = 0;
    while (done < length) {
        const long n = write(fd, bytes + done,
                             static_cast<unsigned long>(length - done));
        if (n <= 0) return false;
        done += static_cast<unsigned long long>(n);
    }
    return true;
}

static bool WriteDecimal(int fd, unsigned long long value) {
    char digits[32]{};
    UINTN count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value && count < sizeof(digits));
    for (UINTN i = 0; i < count / 2; ++i) {
        const char t = digits[i];
        digits[i] = digits[count - 1 - i];
        digits[count - 1 - i] = t;
    }
    return WriteAll(fd, digits, count);
}

static int Error(const char* message, int code) {
    unsigned long long n = 0;
    while (message[n]) ++n;
    (void)WriteAll(2, message, n);
    (void)WriteAll(2, "\n", 1);
    return code;
}

static bool ReadWholeFile(const char* path, uint8_t* buffer,
                          unsigned long long capacity,
                          unsigned long long* sizeOut) {
    if (!path || !buffer || !sizeOut) return false;
    *sizeOut = 0;
    const int fd = open(path, 0);
    if (fd < 0) return false;
    const long size = lseek(fd, 0, 2);
    if (size <= 0 || static_cast<unsigned long long>(size) > capacity ||
        lseek(fd, 0, 0) < 0) {
        (void)close(fd);
        return false;
    }
    unsigned long long done = 0;
    while (done < static_cast<unsigned long long>(size)) {
        const long n = read(fd, buffer + done,
                            static_cast<unsigned long>(
                                static_cast<unsigned long long>(size) - done));
        if (n <= 0) {
            (void)close(fd);
            return false;
        }
        done += static_cast<unsigned long long>(n);
    }
    if (close(fd) != 0) return false;
    *sizeOut = done;
    return true;
}

struct Phase13KextEnumerationCheck {
    const char* ExpectedName;
    uint32_t EntryCount;
    uint64_t FileId;
    bool Found;
    bool IsDirectory;
};

static bool Phase13CStringEqual(const char* a, const char* b) {
    if (!a || !b) return false;
    for (uint32_t i = 0; i < 256u; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return false;
}

static UINTN Phase13StringLength(const char* value, UINTN capacity) {
    if (!value) return capacity;
    UINTN length = 0;
    while (length < capacity && value[length]) ++length;
    return length;
}

static EFI_STATUS Phase13KextEnumerationVisitor(
    void* opaque, const APFS_DIRECTORY_ENTRY* entry) {
    if (!opaque || !entry) return EFI_INVALID_PARAMETER;
    auto* state = static_cast<Phase13KextEnumerationCheck*>(opaque);
    if (state->EntryCount == 0xFFFFFFFFu) return EFI_LOAD_ERROR;
    ++state->EntryCount;
    if (Phase13CStringEqual(entry->Name, state->ExpectedName)) {
        state->Found = true;
        state->FileId = entry->FileId;
        state->IsDirectory = entry->IsDirectory;
    }
    return EFI_SUCCESS;
}

static UINTN Phase13FindBytes(
    const uint8_t* bytes, UINTN size, const char* token, UINTN tokenSize,
    UINTN start) {
    if (!bytes || !token || tokenSize == 0 || start > size ||
        tokenSize > size - start) return size;
    for (UINTN i = start; i <= size - tokenSize; ++i) {
        UINTN j = 0;
        while (j < tokenSize && bytes[i + j] == static_cast<uint8_t>(token[j]))
            ++j;
        if (j == tokenSize) return i;
    }
    return size;
}

static bool Phase13PlistExecutableMatches(
    const uint8_t* bytes, UINTN size, const char* expectedName) {
    static const char key[] = "<key>StarlixExecutable</key>";
    static const char open[] = "<string>";
    static const char close[] = "</string>";
    if (!bytes || !size || !expectedName) return false;
    const UINTN keyAt = Phase13FindBytes(bytes, size, key, sizeof(key) - 1, 0);
    if (keyAt == size || Phase13FindBytes(
            bytes, size, key, sizeof(key) - 1, keyAt + sizeof(key) - 1) != size)
        return false;
    UINTN cursor = keyAt + sizeof(key) - 1;
    while (cursor < size && (bytes[cursor] == ' ' || bytes[cursor] == '\t' ||
           bytes[cursor] == '\r' || bytes[cursor] == '\n')) ++cursor;
    if (Phase13FindBytes(bytes, size, open, sizeof(open) - 1, cursor) != cursor)
        return false;
    const UINTN valueAt = cursor + sizeof(open) - 1;
    const UINTN valueEnd = Phase13FindBytes(
        bytes, size, close, sizeof(close) - 1, valueAt);
    if (valueEnd == size || valueEnd <= valueAt ||
        valueEnd - valueAt >= 128U) return false;
    char value[128]{};
    for (UINTN i = valueAt; i < valueEnd; ++i)
        value[i - valueAt] = static_cast<char>(bytes[i]);
    return Phase13CStringEqual(value, expectedName) &&
        Phase12ValidKextComponentName(value, ".sys");
}

static bool Phase13BuildKextPath(
    char* output, UINTN capacity, const char* bundleName,
    const char* suffix, const char* executableName = nullptr) {
    if (!output || capacity == 0 || !bundleName || !suffix) return false;
    const char* prefix = "/System/Library/Extensions/";
    UINTN pos = 0;
    auto append = [&](const char* part) -> bool {
        if (!part) return true;
        UINTN n = 0;
        while (part[n]) ++n;
        if (n >= capacity - pos) return false;
        for (UINTN i = 0; i < n; ++i) output[pos++] = part[i];
        output[pos] = '\0';
        return true;
    };
    output[0] = '\0';
    if (!append(prefix) || !append(bundleName) || !append(suffix) ||
        !append(executableName)) return false;
    return true;
}

int main(int argc, char** argv) {
    const bool realKextMode = (argc == 9);
    if (argc != 5 && argc != 6 && !realKextMode) {
        return Error(
            "Usage: phase12_fixture_export <boot.efi> <kernel.ect> <driver.sys> <output-rxfs.img> [stlcs.ect] [bundle.kext executable.sys Info.plist]",
            2);
    }

    if ((argc == 6 || realKextMode) &&
        argv[5][0] != '-' ) {
        if (!ReadWholeFile(argv[5], E2eStlcsImage, sizeof(E2eStlcsImage),
                           &E2eStlcsSize) || E2eStlcsSize < 152u ||
            E2eStlcsImage[0] != 'P' || E2eStlcsImage[1] != 'x')
            return Error("ABORT: cannot read bounded production stlcs.ect", 45);
        const char componentInput[] = "PASS: bounded stlcs.ect source loaded for RXFS fixture\n";
        (void)WriteAll(1, componentInput, sizeof(componentInput) - 1);
    }

    const char* bundleName = realKextMode ? argv[6] : "example.kext";
    const char* executableName = realKextMode ? argv[7] : "ExampleKext.sys";
    const uint8_t* infoPlistBytes =
        reinterpret_cast<const uint8_t*>(kPhase13TestInfoPlist);
    unsigned long long infoPlistSize = sizeof(kPhase13TestInfoPlist) - 1;
    if (!Phase12ValidKextComponentName(bundleName, ".kext") ||
        !Phase12ValidKextComponentName(executableName, ".sys"))
        return Error("ABORT: invalid KEXT bundle or executable component name", 46);
    if (realKextMode) {
        if (!ReadWholeFile(argv[8], gInfoPlistBuffer,
                           sizeof(gInfoPlistBuffer), &infoPlistSize) ||
            !Phase13PlistExecutableMatches(gInfoPlistBuffer,
                static_cast<UINTN>(infoPlistSize), executableName))
            return Error("ABORT: real KEXT Info.plist does not declare the requested executable", 47);
        infoPlistBytes = gInfoPlistBuffer;
        const char realInput[] = "PASS: real KEXT Info.plist loaded and executable binding verified\n";
        (void)WriteAll(1, realInput, sizeof(realInput) - 1);
    }

    unsigned long long bootSize = 0;
    unsigned long long ectSize = 0;
    unsigned long long dbSize = 0;
    if (!ReadWholeFile(argv[1], gBootBuffer, sizeof(gBootBuffer), &bootSize))
        return Error("ABORT: cannot read boot.efi or image exceeds 4 MiB", 3);
    if (!ReadWholeFile(argv[2], gEctBuffer, sizeof(gEctBuffer), &ectSize))
        return Error("ABORT: cannot read production ECT or image exceeds 4 MiB", 4);
    if (!ReadWholeFile(argv[3], gDbBuffer, sizeof(gDbBuffer), &dbSize))
        return Error("ABORT: cannot read valid SYS test package", 32);

    if (!BuildPhase12Image(gBootBuffer, bootSize, gEctBuffer, ectSize,
                           gDbBuffer, dbSize, infoPlistBytes, infoPlistSize,
                           bundleName, executableName))
        return Error("ABORT: failed to construct APFS fixture with SYS package", 5);


  // Host-side APFS self-check: catch path/tree errors before building the VM disk.
  APFS_SOURCE checkSource{ReadImage, Image, sizeof(Image)};
  APFS_VOLUME_CONTEXT checkVolume{};
  EFI_STATUS checkStatus = ApfsOpenVolume(&checkSource, 0, &checkVolume);
  if (checkStatus != EFI_SUCCESS)
      return Error("ABORT: APFS fixture volume failed to open", 9);

  APFS_PATH_RESULT bootPath{};
  checkStatus = ApfsLookupPath(
      &checkVolume, "/System/Library/CoreServices/boot.efi", &bootPath);
  if (checkStatus != EFI_SUCCESS || bootPath.FileId != 13 || bootPath.IsDirectory)
      return Error("ABORT: APFS boot.efi path lookup failed", 10);

  APFS_FILE_INFO bootInfo{};
  checkStatus = ApfsGetFileInfo(&checkVolume, bootPath.FileId, &bootInfo);
  if (checkStatus != EFI_SUCCESS || bootInfo.Size != bootSize ||
      (bootInfo.Mode & 0xF000U) != 0x8000U)
      return Error("ABORT: APFS boot.efi metadata mismatch", 11);

  // The graphical RXLOAD image exceeds the old 64 KiB fixture. Verify its
  // complete file stream in bounded chunks, not just the APFS inode metadata.
  uint8_t bootVerifyChunk[4096]{};
  uint64_t bootVerifyOffset = 0;
  while (bootVerifyOffset < bootSize) {
      const UINTN request = static_cast<UINTN>(
          (bootSize - bootVerifyOffset > sizeof(bootVerifyChunk))
              ? sizeof(bootVerifyChunk) : (bootSize - bootVerifyOffset));
      UINTN bytesRead = 0;
      checkStatus = ApfsReadFile(
          &checkVolume, bootPath.FileId, bootVerifyOffset,
          request, bootVerifyChunk, &bytesRead);
      if (checkStatus != EFI_SUCCESS || bytesRead != request)
          return Error("ABORT: APFS full boot.efi readback failed", 37);
      for (UINTN i = 0; i < request; ++i) {
          if (bootVerifyChunk[i] != gBootBuffer[bootVerifyOffset + i])
              return Error("ABORT: APFS boot.efi readback differs from source", 38);
      }
      bootVerifyOffset += request;
  }
  const char bootReadbackPass[] =
      "PASS: complete boot.efi payload APFS readback matches source\n";
  (void)WriteAll(1, bootReadbackPass, sizeof(bootReadbackPass) - 1);

  APFS_PATH_RESULT starlixPath{};
  checkStatus = ApfsLookupPath(&checkVolume, "/Starlix", &starlixPath);
  if (checkStatus != EFI_SUCCESS)
      return Error("ABORT: /Starlix lookup returned an error status", 12);
  if (starlixPath.FileId != 20)
      return Error("ABORT: /Starlix lookup returned the wrong FileId", 17);
  if (!starlixPath.IsDirectory)
      return Error("ABORT: /Starlix entry is not marked as a directory", 18);

  const char passStarlix[] = "PASS: /Starlix path and directory metadata\n";
  (void)WriteAll(1, passStarlix, sizeof(passStarlix) - 1);


  APFS_PATH_RESULT starlixSystemPath{};
  checkStatus = ApfsLookupPath(
      &checkVolume, "/Starlix/System", &starlixSystemPath);
  if (checkStatus != EFI_SUCCESS || starlixSystemPath.FileId != 21 ||
      !starlixSystemPath.IsDirectory)
      return Error("ABORT: APFS lookup failed at /Starlix/System", 15);

  const char passStarlixSystem[] = "PASS: /Starlix/System path lookup\n";
  (void)WriteAll(1, passStarlixSystem, sizeof(passStarlixSystem) - 1);

  APFS_PATH_RESULT kernelPath{};
  checkStatus = ApfsLookupPath(
      &checkVolume, "/System/Library/Kernels/Kernel", &kernelPath);
  if (checkStatus != EFI_SUCCESS || kernelPath.FileId != 15 ||
      kernelPath.IsDirectory)
      return Error("ABORT: APFS lookup failed at /System/Library/Kernels/Kernel", 16);

  const char passKernelPath[] = "PASS: ECT file path lookup\n";
  (void)WriteAll(1, passKernelPath, sizeof(passKernelPath) - 1);

  APFS_FILE_INFO kernelInfo{};
  checkStatus = ApfsGetFileInfo(&checkVolume, kernelPath.FileId, &kernelInfo);
  if (checkStatus != EFI_SUCCESS || kernelInfo.Size != ectSize ||
      (kernelInfo.Mode & 0xF000U) != 0x8000U)
      return Error("ABORT: APFS kernel ECT metadata mismatch", 13);

  uint8_t ectHeader[128]{};
  UINTN ectBytesRead = 0;
  checkStatus = ApfsReadFile(
      &checkVolume, kernelPath.FileId, 0, sizeof(ectHeader),
      ectHeader, &ectBytesRead);
  if (checkStatus != EFI_SUCCESS || ectBytesRead != sizeof(ectHeader) ||
      ectHeader[0] != 'P' || ectHeader[1] != 'x' ||
      ectHeader[80] != 128 || ectHeader[81] != 0)
      return Error("ABORT: APFS kernel ECT header read failed", 14);

  // Read back every ECT byte through the APFS reader in bounded chunks.
  uint8_t ectVerifyChunk[4096]{};
  uint64_t verifyOffset = 0;
  while (verifyOffset < ectSize) {
      const UINTN request = static_cast<UINTN>(
          (ectSize - verifyOffset > sizeof(ectVerifyChunk))
              ? sizeof(ectVerifyChunk) : (ectSize - verifyOffset));
      UINTN bytesRead = 0;
      checkStatus = ApfsReadFile(
          &checkVolume, kernelPath.FileId, verifyOffset,
          request, ectVerifyChunk, &bytesRead);
      if (checkStatus != EFI_SUCCESS || bytesRead != request)
          return Error("ABORT: APFS full ECT payload readback failed", 19);

      for (UINTN i = 0; i < request; ++i) {
          if (ectVerifyChunk[i] != gEctBuffer[verifyOffset + i])
              return Error("ABORT: APFS ECT readback differs from source", 20);
      }
      verifyOffset += request;
  }

  const char fullEctPass[] =
      "PASS: complete ECT payload APFS readback matches source\n";
  (void)WriteAll(1, fullEctPass, sizeof(fullEctPass) - 1);

  const char apfsPass[] =
      "PASS: APFS volume, boot.efi path, ECT path, metadata and ECT read validated\n";
  (void)WriteAll(1, apfsPass, sizeof(apfsPass) - 1);

  // Phase 13E: verify the selected KEXT bundle and its complete files through
  // the same APFS reader used by the loader. Payloads are never executed here.
  APFS_PATH_RESULT extensionsPath{};
  checkStatus = ApfsLookupPath(
      &checkVolume, "/System/Library/Extensions", &extensionsPath);
  if (checkStatus != EFI_SUCCESS || extensionsPath.FileId != 22 ||
      !extensionsPath.IsDirectory)
      return Error("ABORT: KEXT Extensions directory lookup failed", 21);

  Phase13KextEnumerationCheck enumerationCheck{};
  enumerationCheck.ExpectedName = bundleName;
  checkStatus = ApfsEnumerateDirectory(
      &checkVolume, "/System/Library/Extensions",
      Phase13KextEnumerationVisitor, &enumerationCheck);
  if (checkStatus != EFI_SUCCESS || enumerationCheck.EntryCount != 1 ||
      !enumerationCheck.Found || enumerationCheck.FileId != 23 ||
      !enumerationCheck.IsDirectory)
      return Error("ABORT: selected KEXT bundle directory enumeration failed", 31);

  char bundlePath[512]{};
  char plistPath[640]{};
  char payloadPath[640]{};
  if (!Phase13BuildKextPath(bundlePath, sizeof(bundlePath), bundleName, "") ||
      !Phase13BuildKextPath(plistPath, sizeof(plistPath), bundleName,
                            "/Contents/Info.plist") ||
      !Phase13BuildKextPath(payloadPath, sizeof(payloadPath), bundleName,
                            "/Contents/Starlix/", executableName))
      return Error("ABORT: KEXT APFS path exceeds bounded path storage", 48);

  APFS_PATH_RESULT bundlePathResult{};
  checkStatus = ApfsLookupPath(&checkVolume, bundlePath, &bundlePathResult);
  if (checkStatus != EFI_SUCCESS || bundlePathResult.FileId != 23 ||
      !bundlePathResult.IsDirectory)
      return Error("ABORT: selected KEXT bundle lookup failed", 22);

  APFS_PATH_RESULT plistPathResult{};
  checkStatus = ApfsLookupPath(&checkVolume, plistPath, &plistPathResult);
  if (checkStatus != EFI_SUCCESS || plistPathResult.FileId != 25 ||
      plistPathResult.IsDirectory)
      return Error("ABORT: selected KEXT Info.plist lookup failed", 23);
  APFS_FILE_INFO plistInfo{};
  checkStatus = ApfsGetFileInfo(&checkVolume, plistPathResult.FileId, &plistInfo);
  if (checkStatus != EFI_SUCCESS || plistInfo.Size != infoPlistSize ||
      (plistInfo.Mode & 0xF000U) != 0x8000U)
      return Error("ABORT: selected KEXT Info.plist metadata mismatch", 24);

  uint8_t plistReadback[4096]{};
  uint64_t plistOffset = 0;
  while (plistOffset < infoPlistSize) {
      const UINTN request = static_cast<UINTN>(
          (infoPlistSize - plistOffset > sizeof(plistReadback))
              ? sizeof(plistReadback) : (infoPlistSize - plistOffset));
      UINTN bytesRead = 0;
      checkStatus = ApfsReadFile(&checkVolume, plistPathResult.FileId,
          plistOffset, request, plistReadback, &bytesRead);
      if (checkStatus != EFI_SUCCESS || bytesRead != request)
          return Error("ABORT: selected KEXT Info.plist readback failed", 25);
      for (UINTN i = 0; i < request; ++i)
          if (plistReadback[i] != infoPlistBytes[plistOffset + i])
              return Error("ABORT: selected KEXT Info.plist bytes differ", 26);
      plistOffset += request;
  }

  APFS_PATH_RESULT dbPathResult{};
  checkStatus = ApfsLookupPath(&checkVolume, payloadPath, &dbPathResult);
  if (checkStatus != EFI_SUCCESS || dbPathResult.FileId != 27 ||
      dbPathResult.IsDirectory)
      return Error("ABORT: selected KEXT SYS payload lookup failed", 27);
  APFS_FILE_INFO dbInfo{};
  checkStatus = ApfsGetFileInfo(&checkVolume, dbPathResult.FileId, &dbInfo);
  if (checkStatus != EFI_SUCCESS || dbInfo.Size != dbSize ||
      (dbInfo.Mode & 0xF000U) != 0x8000U)
      return Error("ABORT: selected KEXT SYS payload metadata mismatch", 28);

  uint8_t dbReadback[4096]{};
  uint64_t dbOffset = 0;
  while (dbOffset < dbSize) {
      const UINTN request = static_cast<UINTN>(
          (dbSize - dbOffset > sizeof(dbReadback))
              ? sizeof(dbReadback) : (dbSize - dbOffset));
      UINTN bytesRead = 0;
      checkStatus = ApfsReadFile(&checkVolume, dbPathResult.FileId,
          dbOffset, request, dbReadback, &bytesRead);
      if (checkStatus != EFI_SUCCESS || bytesRead != request)
          return Error("ABORT: selected KEXT SYS payload readback failed", 29);
      for (UINTN i = 0; i < request; ++i)
          if (dbReadback[i] != gDbBuffer[dbOffset + i])
              return Error("ABORT: selected KEXT SYS payload bytes differ", 30);
      dbOffset += request;
  }

  if (dbSize > BS) {
      const char kextPass[] =
          "PASS: KEXT bundle discovered; Info.plist and multi-block SYS payload read back byte-exact; entry not executed\n";
      (void)WriteAll(1, kextPass, sizeof(kextPass) - 1);
  } else {
      const char kextPass[] =
          "PASS: KEXT bundle discovered; Info.plist and SYS payload read back byte-exact; entry not executed\n";
      (void)WriteAll(1, kextPass, sizeof(kextPass) - 1);
  }
  const char kextNamePrefix[] = "KEXT bundle: ";
  (void)WriteAll(1, kextNamePrefix, sizeof(kextNamePrefix) - 1);
  (void)WriteAll(1, bundleName, Phase13StringLength(bundleName, 256U));
  (void)WriteAll(1, "\n", 1);
  const char payloadSummary[] = "KEXT SYS payload bytes=";
  (void)WriteAll(1, payloadSummary, sizeof(payloadSummary) - 1);
  (void)WriteDecimal(1, dbSize);
  const char blockSummary[] = "; APFS extent blocks=";
  (void)WriteAll(1, blockSummary, sizeof(blockSummary) - 1);
  (void)WriteDecimal(1, (dbSize + BS - 1) / BS);
  const char executionDisabled[] = "; entry execution DISABLED\n";
  (void)WriteAll(1, executionDisabled, sizeof(executionDisabled) - 1);

    E2eBuildDmg(false);
    if (E2eDmgSize <= sizeof(Image) || E2eDmgSize > sizeof(E2eDmgImage))
        return Error("ABORT: synthetic UDIF DMG exceeds configured capacity", 6);

    E2eBuildRxfs();

    // Re-open the serialized 16 MiB RXFS image and stream the entire UDIF
    // file back. This deliberately crosses the 512-entry single-indirect
    // boundary and verifies the double-indirect map, not just the APFS RAM
    // fixture used by the earlier checks.
    RXFS_SOURCE rxfsSource{E2eReadRxfs, E2eRxfsImage, sizeof(E2eRxfsImage)};
    static RXFS_CONTEXT rxfsCheck{};
    if (RxfsMountSource(&rxfsSource, &rxfsCheck) != EFI_SUCCESS)
        return Error("ABORT: enlarged RXFS fixture failed to mount", 33);
    RXFS_FILE dmgFile{};
    if (RxfsOpen(&rxfsCheck, "/sys/BaseSystem.dmg", &dmgFile) != EFI_SUCCESS ||
        dmgFile.Inode.file_size != E2eDmgSize)
        return Error("ABORT: RXFS BaseSystem.dmg metadata mismatch", 34);
    uint8_t rxfsVerifyChunk[4096]{};
    uint64_t rxfsVerifyOffset = 0;
    while (rxfsVerifyOffset < E2eDmgSize) {
        const UINTN request = static_cast<UINTN>(
            (E2eDmgSize - rxfsVerifyOffset > sizeof(rxfsVerifyChunk))
                ? sizeof(rxfsVerifyChunk) : (E2eDmgSize - rxfsVerifyOffset));
        if (RxfsReadFileRange(&rxfsCheck, &dmgFile, rxfsVerifyOffset,
                              request, rxfsVerifyChunk) != EFI_SUCCESS)
            return Error("ABORT: RXFS double-indirect read failed", 35);
        for (UINTN i = 0; i < request; ++i)
            if (rxfsVerifyChunk[i] != E2eDmgImage[rxfsVerifyOffset + i])
                return Error("ABORT: RXFS BaseSystem.dmg readback differs", 36);
        rxfsVerifyOffset += request;
    }
    const char rxfsReadPass[] =
        "PASS: complete BaseSystem.dmg RXFS readback matches across indirect and double-indirect blocks\n";
    (void)WriteAll(1, rxfsReadPass, sizeof(rxfsReadPass) - 1);

    if (E2eStlcsSize != 0u) {
        RXFS_FILE stlcsFile{};
        if (RxfsOpen(&rxfsCheck, "/sys/User/stlcs.ect", &stlcsFile) != EFI_SUCCESS ||
            stlcsFile.Inode.file_size != E2eStlcsSize ||
            stlcsFile.Inode.file_type != starlix::rxfs::RXFS_TYPE_REGULAR)
            return Error("ABORT: RXFS stlcs.ect path or metadata validation failed", 42);
        uint8_t stlcsReadback[sizeof(E2eStlcsImage)]{};
        if (RxfsReadFileRange(&rxfsCheck, &stlcsFile, 0, E2eStlcsSize,
                              stlcsReadback) != EFI_SUCCESS)
            return Error("ABORT: RXFS stlcs.ect readback failed", 43);
        for (uint64_t i = 0; i < E2eStlcsSize; ++i)
            if (stlcsReadback[i] != E2eStlcsImage[i])
                return Error("ABORT: RXFS stlcs.ect bytes differ from source", 44);
        const char stlcsPass[] =
            "PASS: RXFS /sys/User/stlcs.ect path lookup and byte-exact readback\n";
        (void)WriteAll(1, stlcsPass, sizeof(stlcsPass) - 1);
    }

    static UDIF_CONTEXT udifCheck{};
    static STBOOT_BASESYSTEM_CONTEXT baseCheck{};
    if (StbootOpenBaseSystem(&rxfsCheck, 0, &udifCheck, &baseCheck) != EFI_SUCCESS ||
        !baseCheck.Opened)
        return Error("ABORT: RXFS -> UDIF -> APFS BaseSystem open failed", 37);
    APFS_PATH_RESULT kernelPathViaRxfs{};
    if (ApfsLookupPath(&baseCheck.Volume, "/System/Library/Kernels/Kernel",
                       &kernelPathViaRxfs) != EFI_SUCCESS ||
        kernelPathViaRxfs.IsDirectory)
        return Error("ABORT: kernel ECT path lookup through RXFS failed", 38);
    APFS_FILE_INFO kernelInfoViaRxfs{};
    if (ApfsGetFileInfo(&baseCheck.Volume, kernelPathViaRxfs.FileId,
                        &kernelInfoViaRxfs) != EFI_SUCCESS ||
        kernelInfoViaRxfs.Size != ectSize)
        return Error("ABORT: kernel ECT metadata through RXFS mismatch", 39);
    uint64_t fullPathOffset = 0;
    while (fullPathOffset < ectSize) {
        const UINTN request = static_cast<UINTN>(
            (ectSize - fullPathOffset > sizeof(rxfsVerifyChunk))
                ? sizeof(rxfsVerifyChunk) : (ectSize - fullPathOffset));
        UINTN bytesRead = 0;
        if (ApfsReadFile(&baseCheck.Volume, kernelPathViaRxfs.FileId,
                         fullPathOffset, request, rxfsVerifyChunk,
                         &bytesRead) != EFI_SUCCESS || bytesRead != request)
            return Error("ABORT: kernel ECT read through RXFS/UDIF/APFS failed", 40);
        for (UINTN i = 0; i < request; ++i)
            if (rxfsVerifyChunk[i] != gEctBuffer[fullPathOffset + i])
                return Error("ABORT: kernel ECT RXFS/UDIF/APFS bytes differ", 41);
        fullPathOffset += request;
    }
    const char fullStackPass[] =
        "PASS: full production ECT byte-readback matches through RXFS -> UDIF -> APFS\n";
    (void)WriteAll(1, fullStackPass, sizeof(fullStackPass) - 1);

    const int out = open(argv[4], 1 | 0100 | 01000, 0644); // O_WRONLY|O_CREAT|O_TRUNC
    if (out < 0) return Error("ABORT: cannot create RXFS fixture", 7);
    const bool ok = WriteAll(out, E2eRxfsImage, sizeof(E2eRxfsImage));
    const int closeStatus = close(out);
    if (!ok || closeStatus != 0)
        return Error("ABORT: failed to write RXFS fixture", 8);

    const char successWithStlcs[] =
        "PASS: Phase 12 RXFS fixture contains boot.efi, validated kernel ECT, and stlcs.ect\n";
    const char successWithoutStlcs[] =
        "PASS: Phase 12 RXFS fixture contains boot.efi and validated ECT kernel file\n";
    const char* success = E2eStlcsSize != 0u ? successWithStlcs : successWithoutStlcs;
    unsigned long long successLength = 0;
    while (success[successLength]) ++successLength;
    (void)WriteAll(1, success, successLength);
    return 0;
}
