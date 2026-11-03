#pragma once

#if !defined(STARLIX_PRIMITIVE_TYPES_DEFINED)
#include <stddef.h>
#include <stdint.h>
#endif
#include "sys_validate.hpp"

namespace starlix::sys {

// Phase 13C: bounded, fail-closed relocation of the current x86-64 ET_REL
// driver format. This core prepares bytes in a caller-owned writable buffer;
// it does NOT allocate executable kernel pages or invoke the entry point.
constexpr uint32_t SYS_ELF_LOAD_MAX_SECTIONS = 128;
constexpr uint32_t SYS_ELF_LOAD_MAX_RELOCATIONS = 4096;
constexpr uint64_t SYS_ELF_LOAD_MAX_IMAGE_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t SYS_ELF_LOAD_MAX_ALIGNMENT = 2ULL * 1024ULL * 1024ULL;

constexpr uint32_t SYS_SHT_PROGBITS = 1;
constexpr uint32_t SYS_SHT_SYMTAB = 2;
constexpr uint32_t SYS_SHT_STRTAB = 3;
constexpr uint32_t SYS_SHT_RELA = 4;
constexpr uint32_t SYS_SHT_NOBITS = 8;
constexpr uint32_t SYS_SHT_REL = 9;

constexpr uint64_t SYS_SHF_WRITE = 0x1ULL;
constexpr uint64_t SYS_SHF_ALLOC = 0x2ULL;
constexpr uint64_t SYS_SHF_EXECINSTR = 0x4ULL;
constexpr uint64_t SYS_SHF_TLS = 0x400ULL;
constexpr uint64_t SYS_SHF_COMPRESSED = 0x800ULL;

constexpr uint16_t SYS_SHN_UNDEF = 0;
constexpr uint16_t SYS_SHN_ABS = 0xfff1;
constexpr uint16_t SYS_SHN_COMMON = 0xfff2;
constexpr uint16_t SYS_SHN_XINDEX = 0xffff;

constexpr uint8_t SYS_STB_GLOBAL = 1;
constexpr uint8_t SYS_STT_FUNC = 2;
constexpr uint8_t SYS_STT_TLS = 6;
constexpr uint8_t SYS_STT_GNU_IFUNC = 10;
constexpr uint8_t SYS_STV_DEFAULT = 0;

constexpr uint32_t SYS_R_X86_64_64 = 1;

// The result is deliberately distinct from SYS_VALIDATE_RESULT: a well-formed
// STSYS001 package can still contain an object this relocator does not support.
enum class SYS_ELF_LOAD_RESULT : uint32_t {
    Valid = 0,
    InvalidArgument,
    PackageInvalid,
    SectionLimit,
    SectionType,
    SectionFlags,
    SectionAlignment,
    ImageTooLarge,
    ImageBufferTooSmall,
    ImageBufferOverlap,
    MetadataBufferOverlap,
    ImageBaseMismatch,
    ImageBaseAlignment,
    SymbolTable,
    EntrySymbol,
    EntrySymbolType,
    UndefinedSymbol,
    UnsupportedSymbol,
    RelocationSection,
    UnsupportedRelocation,
    RelocationBounds,
    RelocationOverflow,
    TooManyRelocations
};

struct SYS_ELF_LOADED_SECTION {
    uint16_t section_index;
    uint16_t reserved;
    uint32_t reserved2;
    uint64_t image_offset;
    uint64_t size;
    uint64_t flags;
};

struct SYS_ELF_LOAD_IMAGE {
    uint64_t image_size;
    uint64_t image_alignment;
    uint64_t entry_offset;
    uint64_t entry_address;
    uint32_t loaded_section_count;
    uint32_t applied_relocation_count;
    SYS_ELF_LOADED_SECTION sections[SYS_ELF_LOAD_MAX_SECTIONS];
};

inline bool SysElfLoadAddU64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out || a > (~0ULL) - b) return false;
    *out = a + b;
    return true;
}

inline bool SysElfLoadAddSigned(
    uint64_t value, int64_t addend, uint64_t* out) {
    if (!out) return false;
    if (addend >= 0)
        return SysElfLoadAddU64(value, static_cast<uint64_t>(addend), out);

    // Avoid negating INT64_MIN directly.
    const uint64_t magnitude =
        static_cast<uint64_t>(-(addend + 1)) + 1ULL;
    if (value < magnitude) return false;
    *out = value - magnitude;
    return true;
}

inline bool SysElfLoadRangesOverlap(
    uint64_t aBase, uint64_t aSize, uint64_t bBase, uint64_t bSize) {
    uint64_t aEnd = 0;
    uint64_t bEnd = 0;
    if (!SysElfLoadAddU64(aBase, aSize, &aEnd) ||
        !SysElfLoadAddU64(bBase, bSize, &bEnd))
        return true;
    return aBase < bEnd && bBase < aEnd;
}

inline bool SysElfLoadAlignUp(
    uint64_t value, uint64_t alignment, uint64_t* out) {
    if (!out || alignment == 0 || (alignment & (alignment - 1ULL)) != 0)
        return false;
    const uint64_t mask = alignment - 1ULL;
    if (value > (~0ULL) - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

inline const SYS_ELF64_SECTION* SysElfLoadSectionAt(
    const uint8_t* payload, const SYS_ELF64_HEADER* elf, uint16_t index) {
    if (!payload || !elf || index >= elf->shnum) return nullptr;
    return SysElfSectionAt(payload, elf, index);
}

inline const SYS_ELF64_SYMBOL* SysElfLoadSymbolAt(
    const uint8_t* payload, const SYS_ELF64_SECTION* symtab,
    uint64_t symbolIndex) {
    if (!payload || !symtab ||
        symtab->entsize != sizeof(SYS_ELF64_SYMBOL) ||
        symbolIndex >= symtab->size / sizeof(SYS_ELF64_SYMBOL))
        return nullptr;
    return reinterpret_cast<const SYS_ELF64_SYMBOL*>(
        payload + symtab->offset + symbolIndex * sizeof(SYS_ELF64_SYMBOL));
}

inline void SysElfLoadWriteU64LE(uint8_t* destination, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        destination[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

inline SYS_ELF_LOAD_RESULT SysElfLoadComputeRelocation(
    const uint8_t* payload, const SYS_ELF64_HEADER* elf,
    const SYS_ELF64_SECTION* symtab, uint64_t symbolCount,
    uint16_t targetIndex, const SYS_ELF64_RELA* relocation,
    uint64_t loadAddress, uint64_t imageEnd,
    const uint64_t* sectionOffsets, const bool* sectionLoaded,
    uint64_t* targetOffsetOut, uint64_t* relocatedValueOut) {
    if (!payload || !elf || !symtab || !relocation || !sectionOffsets ||
        !sectionLoaded || !targetOffsetOut || !relocatedValueOut ||
        targetIndex >= elf->shnum)
        return SYS_ELF_LOAD_RESULT::InvalidArgument;

    const uint32_t relocationType =
        static_cast<uint32_t>(relocation->info & 0xffffffffULL);
    const uint32_t symbolIndex =
        static_cast<uint32_t>(relocation->info >> 32);
    if (relocationType != SYS_R_X86_64_64)
        return SYS_ELF_LOAD_RESULT::UnsupportedRelocation;
    if (symbolIndex >= symbolCount)
        return SYS_ELF_LOAD_RESULT::RelocationSection;

    const auto* targetSection = SysElfLoadSectionAt(payload, elf, targetIndex);
    if (!targetSection || !sectionLoaded[targetIndex] ||
        relocation->offset > targetSection->size ||
        sizeof(uint64_t) > targetSection->size - relocation->offset)
        return SYS_ELF_LOAD_RESULT::RelocationBounds;

    const auto* symbol = SysElfLoadSymbolAt(payload, symtab, symbolIndex);
    if (!symbol) return SYS_ELF_LOAD_RESULT::SymbolTable;
    if (symbol->shndx == SYS_SHN_UNDEF)
        return SYS_ELF_LOAD_RESULT::UndefinedSymbol;
    if (symbol->shndx == SYS_SHN_XINDEX ||
        symbol->shndx == SYS_SHN_COMMON ||
        (symbol->shndx >= elf->shnum && symbol->shndx != SYS_SHN_ABS))
        return SYS_ELF_LOAD_RESULT::UnsupportedSymbol;
    const uint8_t symbolType = symbol->info & 0x0f;
    if (symbolType == SYS_STT_TLS || symbolType == SYS_STT_GNU_IFUNC)
        return SYS_ELF_LOAD_RESULT::UnsupportedSymbol;

    uint64_t symbolAddress = symbol->value;
    if (symbol->shndx != SYS_SHN_ABS) {
        if (!sectionLoaded[symbol->shndx])
            return SYS_ELF_LOAD_RESULT::UndefinedSymbol;
        const auto* definingSection =
            SysElfLoadSectionAt(payload, elf, symbol->shndx);
        if (!definingSection || symbol->value > definingSection->size)
            return SYS_ELF_LOAD_RESULT::UnsupportedSymbol;
        if (symbolType != 3 /* STT_SECTION */ &&
            symbol->size > definingSection->size - symbol->value)
            return SYS_ELF_LOAD_RESULT::UnsupportedSymbol;

        uint64_t sectionAddress = 0;
        if (!SysElfLoadAddU64(loadAddress,
                sectionOffsets[symbol->shndx], &sectionAddress) ||
            !SysElfLoadAddU64(sectionAddress,
                symbol->value, &symbolAddress))
            return SYS_ELF_LOAD_RESULT::RelocationOverflow;
    }

    uint64_t relocatedValue = 0;
    if (!SysElfLoadAddSigned(
            symbolAddress, relocation->addend, &relocatedValue))
        return SYS_ELF_LOAD_RESULT::RelocationOverflow;
    // This driver is self-contained: external absolute addresses/imports are
    // not part of the current ABI, so every relocated pointer must point back
    // into this module image.
    if (relocatedValue < loadAddress || relocatedValue >= imageEnd)
        return SYS_ELF_LOAD_RESULT::UnsupportedSymbol;

    uint64_t targetOffset = 0;
    if (!SysElfLoadAddU64(
            sectionOffsets[targetIndex], relocation->offset, &targetOffset))
        return SYS_ELF_LOAD_RESULT::RelocationOverflow;
    *targetOffsetOut = targetOffset;
    *relocatedValueOut = relocatedValue;
    return SYS_ELF_LOAD_RESULT::Valid;
}

inline SYS_ELF_LOAD_RESULT RelocateSysElf64Image(
    const uint8_t* package, uint64_t packageSize,
    uint8_t* imageBuffer, uint64_t imageBufferSize,
    uint64_t loadAddress, SYS_ELF_LOAD_IMAGE* result) {
    if (!package || !imageBuffer || !result || packageSize < sizeof(SYS_HEADER))
        return SYS_ELF_LOAD_RESULT::InvalidArgument;

    const uint64_t packageAddress = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(package));
    const uint64_t imageAddress = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(imageBuffer));
    const uint64_t resultAddress = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(result));
    if (SysElfLoadRangesOverlap(packageAddress, packageSize,
            imageAddress, imageBufferSize))
        return SYS_ELF_LOAD_RESULT::ImageBufferOverlap;
    if (SysElfLoadRangesOverlap(resultAddress, sizeof(*result),
            packageAddress, packageSize) ||
        SysElfLoadRangesOverlap(resultAddress, sizeof(*result),
            imageAddress, imageBufferSize))
        return SYS_ELF_LOAD_RESULT::MetadataBufferOverlap;

    const SYS_VALIDATE_RESULT packageValidation =
        ValidateSysPackage(package, packageSize);
    if (packageValidation == SYS_VALIDATE_RESULT::EntrySymbol)
        return SYS_ELF_LOAD_RESULT::EntrySymbol;
    if (packageValidation != SYS_VALIDATE_RESULT::Valid)
        return SYS_ELF_LOAD_RESULT::PackageInvalid;

    const auto* sysHeader = reinterpret_cast<const SYS_HEADER*>(package);
    const uint8_t* payload = package + sysHeader->header_size;
    const auto* elf = reinterpret_cast<const SYS_ELF64_HEADER*>(payload);

    if (elf->shnum == 0 || elf->shnum > SYS_ELF_LOAD_MAX_SECTIONS)
        return SYS_ELF_LOAD_RESULT::SectionLimit;

    // The host-test API and the future kernel caller must use the actual
    // virtual address at which the writable image buffer will be mapped.
    if (loadAddress != static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(imageBuffer)))
        return SYS_ELF_LOAD_RESULT::ImageBaseMismatch;

    uint64_t sectionOffsets[SYS_ELF_LOAD_MAX_SECTIONS]{};
    bool sectionLoaded[SYS_ELF_LOAD_MAX_SECTIONS]{};
    uint64_t imageCursor = 0;
    uint64_t maxAlignment = 1;
    uint32_t loadedSectionCount = 0;
    bool hasExecutableSection = false;

    // Lay out only SHF_ALLOC sections. Debug information and its relocations
    // remain in the package and are never copied into the runtime image.
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* section = SysElfLoadSectionAt(payload, elf, i);
        if (!section) return SYS_ELF_LOAD_RESULT::SectionType;
        if ((section->flags & SYS_SHF_ALLOC) == 0) continue;

        if (section->type != SYS_SHT_PROGBITS &&
            section->type != SYS_SHT_NOBITS)
            return SYS_ELF_LOAD_RESULT::SectionType;
        if ((section->flags & (SYS_SHF_TLS | SYS_SHF_COMPRESSED)) != 0 ||
            ((section->flags & SYS_SHF_WRITE) != 0 &&
             (section->flags & SYS_SHF_EXECINSTR) != 0))
            return SYS_ELF_LOAD_RESULT::SectionFlags;

        uint64_t alignment = section->addralign;
        if (alignment == 0) alignment = 1;
        if (alignment > SYS_ELF_LOAD_MAX_ALIGNMENT ||
            (alignment & (alignment - 1ULL)) != 0)
            return SYS_ELF_LOAD_RESULT::SectionAlignment;
        if (alignment > maxAlignment) maxAlignment = alignment;

        uint64_t alignedOffset = 0;
        if (!SysElfLoadAlignUp(imageCursor, alignment, &alignedOffset) ||
            !SysElfLoadAddU64(alignedOffset, section->size, &imageCursor) ||
            imageCursor > SYS_ELF_LOAD_MAX_IMAGE_SIZE)
            return SYS_ELF_LOAD_RESULT::ImageTooLarge;

        sectionOffsets[i] = alignedOffset;
        sectionLoaded[i] = true;
        ++loadedSectionCount;
        if ((section->flags & SYS_SHF_EXECINSTR) != 0 && section->size != 0)
            hasExecutableSection = true;
    }

    if (!loadedSectionCount || !hasExecutableSection)
        return SYS_ELF_LOAD_RESULT::SectionType;
    if (imageBufferSize < imageCursor)
        return SYS_ELF_LOAD_RESULT::ImageBufferTooSmall;
    if ((loadAddress & (maxAlignment - 1ULL)) != 0)
        return SYS_ELF_LOAD_RESULT::ImageBaseAlignment;
    uint64_t imageEnd = 0;
    if (!SysElfLoadAddU64(loadAddress, imageCursor, &imageEnd))
        return SYS_ELF_LOAD_RESULT::RelocationOverflow;
    (void)imageEnd;

    // Find exactly one static symbol table and validate its linked string table.
    uint16_t symtabIndex = 0xffffu;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* section = SysElfLoadSectionAt(payload, elf, i);
        if (section->type == SYS_SHT_SYMTAB) {
            if (symtabIndex != 0xffffu)
                return SYS_ELF_LOAD_RESULT::SymbolTable;
            symtabIndex = i;
        }
        if (section->type == 11) // SHT_DYNSYM is intentionally unsupported.
            return SYS_ELF_LOAD_RESULT::SymbolTable;
    }
    if (symtabIndex == 0xffffu)
        return SYS_ELF_LOAD_RESULT::SymbolTable;

    const auto* symtab = SysElfLoadSectionAt(payload, elf, symtabIndex);
    if (!symtab || symtab->entsize != sizeof(SYS_ELF64_SYMBOL) ||
        symtab->size % sizeof(SYS_ELF64_SYMBOL) != 0 ||
        symtab->link >= elf->shnum)
        return SYS_ELF_LOAD_RESULT::SymbolTable;
    const auto* stringsSection = SysElfLoadSectionAt(
        payload, elf, static_cast<uint16_t>(symtab->link));
    if (!stringsSection || stringsSection->type != SYS_SHT_STRTAB ||
        stringsSection->size == 0)
        return SYS_ELF_LOAD_RESULT::SymbolTable;
    const uint8_t* strings = payload + stringsSection->offset;
    const uint64_t symbolCount = symtab->size / sizeof(SYS_ELF64_SYMBOL);

    // Resolve the declared entry only if it is one unique, global, default-
    // visibility function in a loaded executable section.
    size_t entryNameLength = 0;
    if (!SysFixedTextField(sysHeader->entry_symbol,
            sizeof(sysHeader->entry_symbol), false, &entryNameLength))
        return SYS_ELF_LOAD_RESULT::EntrySymbol;

    uint32_t entryMatches = 0;
    uint64_t entryOffset = 0;
    for (uint64_t i = 0; i < symbolCount; ++i) {
        const auto* symbol = SysElfLoadSymbolAt(payload, symtab, i);
        if (!symbol) return SYS_ELF_LOAD_RESULT::SymbolTable;
        if (!SysElfStringEquals(strings, stringsSection->size,
                symbol->name, sysHeader->entry_symbol, entryNameLength))
            continue;
        ++entryMatches;
        if ((symbol->info >> 4) != SYS_STB_GLOBAL ||
            (symbol->info & 0x0f) != SYS_STT_FUNC ||
            (symbol->other & 0x03) != SYS_STV_DEFAULT ||
            symbol->shndx == SYS_SHN_UNDEF ||
            symbol->shndx >= elf->shnum ||
            !sectionLoaded[symbol->shndx])
            return SYS_ELF_LOAD_RESULT::EntrySymbolType;
        const auto* entrySection = SysElfLoadSectionAt(
            payload, elf, symbol->shndx);
        if (!entrySection ||
            (entrySection->flags & SYS_SHF_EXECINSTR) == 0 ||
            symbol->size == 0 || symbol->value >= entrySection->size ||
            symbol->size > entrySection->size - symbol->value)
            return SYS_ELF_LOAD_RESULT::EntrySymbolType;
        if (!SysElfLoadAddU64(
                sectionOffsets[symbol->shndx], symbol->value, &entryOffset))
            return SYS_ELF_LOAD_RESULT::RelocationOverflow;
    }
    if (entryMatches != 1 || entryOffset >= imageCursor)
        return SYS_ELF_LOAD_RESULT::EntrySymbol;

    if (loadAddress > (~0ULL) - entryOffset)
        return SYS_ELF_LOAD_RESULT::RelocationOverflow;

    // First pass: validate every relocation and compute its value without
    // touching the destination. A malformed late relocation cannot leave a
    // partially prepared image behind.
    uint32_t relocationCount = 0;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* relocationSection = SysElfLoadSectionAt(payload, elf, i);
        if (relocationSection->type != SYS_SHT_RELA &&
            relocationSection->type != SYS_SHT_REL)
            continue;
        if (relocationSection->info >= elf->shnum)
            return SYS_ELF_LOAD_RESULT::RelocationSection;

        const uint16_t targetIndex =
            static_cast<uint16_t>(relocationSection->info);
        if (!sectionLoaded[targetIndex]) continue; // debug/non-alloc relocations
        if (relocationSection->type != SYS_SHT_RELA ||
            relocationSection->link != symtabIndex ||
            relocationSection->entsize != sizeof(SYS_ELF64_RELA) ||
            relocationSection->size % sizeof(SYS_ELF64_RELA) != 0)
            return SYS_ELF_LOAD_RESULT::RelocationSection;

        const uint64_t entries =
            relocationSection->size / sizeof(SYS_ELF64_RELA);
        if (entries > SYS_ELF_LOAD_MAX_RELOCATIONS - relocationCount)
            return SYS_ELF_LOAD_RESULT::TooManyRelocations;
        for (uint64_t r = 0; r < entries; ++r) {
            const auto* relocation = reinterpret_cast<const SYS_ELF64_RELA*>(
                payload + relocationSection->offset +
                r * sizeof(SYS_ELF64_RELA));
            uint64_t ignoredTarget = 0;
            uint64_t ignoredValue = 0;
            const SYS_ELF_LOAD_RESULT relocationResult =
                SysElfLoadComputeRelocation(payload, elf, symtab,
                    symbolCount, targetIndex, relocation, loadAddress,
                    imageEnd, sectionOffsets, sectionLoaded,
                    &ignoredTarget, &ignoredValue);
            if (relocationResult != SYS_ELF_LOAD_RESULT::Valid)
                return relocationResult;
            ++relocationCount;
        }
    }

    // Second pass: all metadata and relocation expressions are known-good.
    // Clear and copy the allocated sections into the caller-owned image.
    for (uint64_t i = 0; i < imageCursor; ++i) imageBuffer[i] = 0;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        if (!sectionLoaded[i]) continue;
        const auto* section = SysElfLoadSectionAt(payload, elf, i);
        if (section->type == SYS_SHT_PROGBITS) {
            const uint8_t* source = payload + section->offset;
            uint8_t* destination = imageBuffer + sectionOffsets[i];
            for (uint64_t byte = 0; byte < section->size; ++byte)
                destination[byte] = source[byte];
        }
        // SHT_NOBITS remains zero from the initial clear.
    }

    uint32_t appliedRelocations = 0;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* relocationSection = SysElfLoadSectionAt(payload, elf, i);
        if (relocationSection->type != SYS_SHT_RELA) continue;
        const uint16_t targetIndex =
            static_cast<uint16_t>(relocationSection->info);
        if (!sectionLoaded[targetIndex]) continue;
        const uint64_t entries =
            relocationSection->size / sizeof(SYS_ELF64_RELA);
        for (uint64_t r = 0; r < entries; ++r) {
            const auto* relocation = reinterpret_cast<const SYS_ELF64_RELA*>(
                payload + relocationSection->offset +
                r * sizeof(SYS_ELF64_RELA));
            uint64_t targetOffset = 0;
            uint64_t relocatedValue = 0;
            const SYS_ELF_LOAD_RESULT relocationResult =
                SysElfLoadComputeRelocation(payload, elf, symtab,
                    symbolCount, targetIndex, relocation, loadAddress,
                    imageEnd, sectionOffsets, sectionLoaded,
                    &targetOffset, &relocatedValue);
            if (relocationResult != SYS_ELF_LOAD_RESULT::Valid) {
                for (uint64_t byte = 0; byte < imageCursor; ++byte)
                    imageBuffer[byte] = 0;
                return relocationResult;
            }
            SysElfLoadWriteU64LE(imageBuffer + targetOffset, relocatedValue);
            ++appliedRelocations;
        }
    }
    if (appliedRelocations != relocationCount) {
        for (uint64_t byte = 0; byte < imageCursor; ++byte)
            imageBuffer[byte] = 0;
        return SYS_ELF_LOAD_RESULT::RelocationSection;
    }

    // Publish output metadata only after all relocations have succeeded.
    // Write directly to caller-owned metadata instead of placing the 4 KiB
    // section map on the kernel stack.
    result->image_size = imageCursor;
    result->image_alignment = maxAlignment;
    result->entry_offset = entryOffset;
    result->entry_address = loadAddress + entryOffset;
    result->loaded_section_count = 0;
    result->applied_relocation_count = appliedRelocations;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        if (!sectionLoaded[i]) continue;
        auto& outSection = result->sections[result->loaded_section_count++];
        outSection = {};
        outSection.section_index = i;
        outSection.image_offset = sectionOffsets[i];
        const auto* section = SysElfLoadSectionAt(payload, elf, i);
        outSection.size = section->size;
        outSection.flags = section->flags;
    }
    return SYS_ELF_LOAD_RESULT::Valid;
}

} // namespace starlix::sys
