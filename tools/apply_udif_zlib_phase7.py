#!/usr/bin/env python3
"""Integrate bounded zlib/DEFLATE support for UDIF chunks into stbootmngr."""
from __future__ import annotations
from datetime import datetime
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
STAMP = datetime.now().strftime("%Y%m%d_%H%M%S")
BACKED_UP: set[Path] = set()


def backup(path: Path) -> None:
    if path in BACKED_UP:
        return
    candidate = path.with_name(path.name + ".bak-udif-zlib-phase7-" + STAMP)
    shutil.copy2(path, candidate)
    BACKED_UP.add(path)
    print(f"Backup : {candidate.relative_to(ROOT)}")


def patch_text(rel: str, transforms: list[tuple[str, str]]) -> None:
    path = ROOT / rel
    if not path.is_file():
        raise SystemExit(f"ABORT: required file missing: {rel}")
    original = path.read_text(encoding="utf-8")
    updated = original
    changed = False
    for old, new in transforms:
        if new in updated:
            continue
        if old in updated:
            if updated.count(old) != 1:
                raise SystemExit(f"ABORT: expected exactly one target in {rel}: {old[:70]!r}")
            updated = updated.replace(old, new, 1)
            changed = True
        else:
            raise SystemExit(f"ABORT: source pattern not found in {rel}: {old[:90]!r}")
    if changed:
        backup(path)
        path.write_text(updated, encoding="utf-8", newline="")
        print(f"Updated: {rel}")


def main() -> int:
    header_transforms = [(
'''enum UDIF_RUN_KIND : uint32_t {
    UDIF_RUN_ZERO = 0,
    UDIF_RUN_STORED = 1,
    UDIF_RUN_UNUSED = 2
};''',
'''enum UDIF_RUN_KIND : uint32_t {
    UDIF_RUN_ZERO = 0,
    UDIF_RUN_STORED = 1,
    UDIF_RUN_UNUSED = 2,
    UDIF_RUN_ZLIB = 3
};''')]
    cpp_transforms = [
        ('#include "udif_reader.hpp"\n', '#include "udif_reader.hpp"\n#include "udif_zlib_inflate.hpp"\n'),
        ('''static uint8_t gXmlScratch[kMaxXmlBytes];
static uint8_t gMishScratch[kMaxDecodedMishBytes];''',
'''static uint8_t gXmlScratch[kMaxXmlBytes];
static uint8_t gMishScratch[kMaxDecodedMishBytes];
static uint8_t gZlibInputScratch[UDIF_ZLIB_MAX_INPUT_BYTES];
static uint8_t gZlibOutputScratch[UDIF_ZLIB_MAX_OUTPUT_BYTES];
static const UDIF_CONTEXT* gZlibCacheContext = nullptr;
static uint32_t gZlibCacheRunIndex = 0;
static bool gZlibCacheValid = false;'''),
        ('''        } else if (Type == kMishTypeStored) {
            uint64_t ForkRelOffset = 0, ForkRelEnd = 0, AbsoluteOffset = 0;
            if (StoredLength != NeededBytes ||
                AddOverflow(TableDataOffset, CompressedOffset, &ForkRelOffset) ||
                AddOverflow(ForkRelOffset, StoredLength, &ForkRelEnd) || ForkRelEnd > DataForkLength ||
                AddOverflow(DataForkOffset, ForkRelOffset, &AbsoluteOffset))
                return EFI_LOAD_ERROR;
            Run.Kind = UDIF_RUN_STORED;
            Run.SourceOffset = AbsoluteOffset;
            Run.StoredBytes = StoredLength;
        } else if (Type == 0x80000004U || Type == kMishTypeZlib ||
                   Type == kMishTypeLzfse || Type == kMishTypeLzma) {
            // Do not treat compressed payload as raw data. Codec support is a later phase.
            return EFI_ERR(3); // EFI_UNSUPPORTED
        } else {''',
'''        } else if (Type == kMishTypeStored || Type == kMishTypeZlib) {
            uint64_t ForkRelOffset = 0, ForkRelEnd = 0, AbsoluteOffset = 0;
            if (AddOverflow(TableDataOffset, CompressedOffset, &ForkRelOffset) ||
                AddOverflow(ForkRelOffset, StoredLength, &ForkRelEnd) || ForkRelEnd > DataForkLength ||
                AddOverflow(DataForkOffset, ForkRelOffset, &AbsoluteOffset))
                return EFI_LOAD_ERROR;
            if (Type == kMishTypeStored) {
                if (StoredLength != NeededBytes) return EFI_LOAD_ERROR;
                Run.Kind = UDIF_RUN_STORED;
            } else {
                if (StoredLength == 0 || StoredLength > UDIF_ZLIB_MAX_INPUT_BYTES ||
                    NeededBytes > UDIF_ZLIB_MAX_OUTPUT_BYTES)
                    return EFI_ERR(3); // EFI_UNSUPPORTED: bounded decoder limit
                Run.Kind = UDIF_RUN_ZLIB;
            }
            Run.SourceOffset = AbsoluteOffset;
            Run.StoredBytes = StoredLength;
        } else if (Type == 0x80000004U || Type == kMishTypeLzfse ||
                   Type == kMishTypeLzma || Type == 0x80000006U) {
            // ADC, bzip2, LZFSE and LZMA remain explicitly unsupported.
            return EFI_ERR(3); // EFI_UNSUPPORTED
        } else {'''),
        ('''    ClearContext(Context);
    ApfsPartitionSource->ReadAt = nullptr;''',
'''    ClearContext(Context);
    gZlibCacheValid = false;
    gZlibCacheContext = nullptr;
    ApfsPartitionSource->ReadAt = nullptr;'''),
        ('''        } else if (Match->Kind == UDIF_RUN_STORED) {
            uint64_t RunStartBytes = 0, SourceOffset = 0;
            if (MulOverflow(Match->FirstSector, UDIF_SECTOR_SIZE, &RunStartBytes) ||
                AddOverflow(Match->SourceOffset, Current - RunStartBytes, &SourceOffset))
                return EFI_LOAD_ERROR;
            EFI_STATUS Status = ReadFileSource(&C->ContainerFile, SourceOffset, (UINTN)Take, Out);
            if (Status != EFI_SUCCESS) return Status;
        } else {
            return EFI_ERR(3);
        }''',
'''        } else if (Match->Kind == UDIF_RUN_STORED) {
            uint64_t RunStartBytes = 0, SourceOffset = 0;
            if (MulOverflow(Match->FirstSector, UDIF_SECTOR_SIZE, &RunStartBytes) ||
                AddOverflow(Match->SourceOffset, Current - RunStartBytes, &SourceOffset))
                return EFI_LOAD_ERROR;
            EFI_STATUS Status = ReadFileSource(&C->ContainerFile, SourceOffset, (UINTN)Take, Out);
            if (Status != EFI_SUCCESS) return Status;
        } else if (Match->Kind == UDIF_RUN_ZLIB) {
            const uint32_t RunIndex = (uint32_t)(Match - C->Runs);
            uint64_t RunStartBytes = 0, ExpandedBytes = 0, InRunOffset = 0;
            if (MulOverflow(Match->FirstSector, UDIF_SECTOR_SIZE, &RunStartBytes) ||
                MulOverflow(Match->SectorCount, UDIF_SECTOR_SIZE, &ExpandedBytes) ||
                ExpandedBytes > UDIF_ZLIB_MAX_OUTPUT_BYTES ||
                Match->StoredBytes == 0 || Match->StoredBytes > UDIF_ZLIB_MAX_INPUT_BYTES)
                return EFI_LOAD_ERROR;
            InRunOffset = Current - RunStartBytes;
            if (InRunOffset > ExpandedBytes || Take > ExpandedBytes - InRunOffset)
                return EFI_LOAD_ERROR;
            if (!gZlibCacheValid || gZlibCacheContext != C || gZlibCacheRunIndex != RunIndex) {
                gZlibCacheValid = false;
                EFI_STATUS Status = ReadFileSource(&C->ContainerFile, Match->SourceOffset,
                                                   (UINTN)Match->StoredBytes, gZlibInputScratch);
                if (Status != EFI_SUCCESS) return Status;
                uint64_t ActualBytes = 0;
                Status = UdifInflateZlib(gZlibInputScratch, Match->StoredBytes,
                                         gZlibOutputScratch, sizeof(gZlibOutputScratch),
                                         ExpandedBytes, &ActualBytes);
                if (Status != EFI_SUCCESS || ActualBytes != ExpandedBytes)
                    return EFI_LOAD_ERROR;
                gZlibCacheContext = C;
                gZlibCacheRunIndex = RunIndex;
                gZlibCacheValid = true;
            }
            for (uint64_t I = 0; I < Take; ++I)
                Out[I] = gZlibOutputScratch[InRunOffset + I];
        } else {
            return EFI_ERR(3);
        }''')]

    patch_text("boot/x64/stbootmngr.efi/src/udif_reader.hpp", header_transforms)
    patch_text("boot/x64/stbootmngr.efi/src/udif_reader.cpp", cpp_transforms)

    build_transforms = [
        ('''clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_reader.cpp" -o "$STBMGR_DIR/build/udif_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_metadata_reader.cpp" -o "$STBMGR_DIR/build/apfs_metadata_reader.o"''',
         '''clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_reader.cpp" -o "$STBMGR_DIR/build/udif_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_zlib_inflate.cpp" -o "$STBMGR_DIR/build/udif_zlib_inflate.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_metadata_reader.cpp" -o "$STBMGR_DIR/build/apfs_metadata_reader.o"'''),
        ('''"$STBMGR_DIR/build/rxfs_reader.o" "$STBMGR_DIR/build/apfs_reader.o" "$STBMGR_DIR/build/udif_reader.o" "$STBMGR_DIR/build/apfs_metadata_reader.o"''',
         '''"$STBMGR_DIR/build/rxfs_reader.o" "$STBMGR_DIR/build/apfs_reader.o" "$STBMGR_DIR/build/udif_reader.o" "$STBMGR_DIR/build/udif_zlib_inflate.o" "$STBMGR_DIR/build/apfs_metadata_reader.o"''')]
    patch_text("scripts/build.sh", build_transforms)

    # Replace the Phase 2 test with the expanded raw/zlib + stored/fixed/dynamic
    # DEFLATE test, keeping the old test as a timestamped backup.
    test_target = ROOT / "tests/stbootmngr_udif_selftest.cpp"
    test_seed = ROOT / "tools/udif_zlib_phase7/stbootmngr_udif_selftest.cpp.new"
    if not test_seed.is_file() or not test_target.is_file():
        raise SystemExit("ABORT: UDIF selftest source/seed missing")
    seed_bytes = test_seed.read_bytes()
    current = test_target.read_bytes()
    if current != seed_bytes:
        if b"compressed zlib chunk: must fail closed" not in current and b"TestFixedAndDynamicDeflate" not in current:
            raise SystemExit("ABORT: existing UDIF selftest differs from known Phase 2 or Phase 7 test")
        if current != seed_bytes:
            backup(test_target)
            test_target.write_bytes(seed_bytes)
            print("Updated: tests/stbootmngr_udif_selftest.cpp")

    # Link the new decoder in every host test that directly compiles udif_reader.cpp.
    test_scripts = [
        "tests/test_stbootmngr_udif.sh",
        "tests/test_stbootmngr_basesystem.sh",
        "tests/test_stbootmngr_apfs_file.sh",
        "tests/test_stbootmngr_apfs_path.sh",
        "tests/test_stbootmngr_apfs_metadata.sh",
    ]
    compile_old = '''  "$ROOT/boot/x64/stbootmngr.efi/src/udif_reader.cpp" \\\n'''
    compile_new = compile_old + '''  "$ROOT/boot/x64/stbootmngr.efi/src/udif_zlib_inflate.cpp" \\\n'''
    for rel in test_scripts:
        patch_text(rel, [(compile_old, compile_new)])

    patch_text("tests/test_stbootmngr_udif.sh", [(
        'echo "PASS: UDIF koly/plist/blkx map + Apple_APFS partition read + NXSB probe + compressed-chunk fail-closed"',
        'echo "PASS: UDIF raw/zlib chunks, stored/fixed/dynamic DEFLATE, Adler-32 and malformed-stream fail-closed"')])

    print("Phase 7 integrated: bounded zlib/DEFLATE UDIF chunks enabled; other codecs remain fail-closed.")
    print("No main.cpp boot-flow changes or physical disk writes were made.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
