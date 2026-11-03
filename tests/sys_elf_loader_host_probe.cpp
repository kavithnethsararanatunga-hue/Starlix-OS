#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "driver/sys_elf_loader.hpp"

using namespace starlix::sys;

static int failures = 0;
static void Check(bool condition, const char* name) {
    if (condition) std::printf("PASS: %s\n", name);
    else { std::printf("FAIL: %s\n", name); ++failures; }
}

static uint64_t ReadU64LE(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

static std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static SYS_ELF64_HEADER* PayloadElf(std::vector<uint8_t>& package) {
    if (package.size() < sizeof(SYS_HEADER)) return nullptr;
    auto* header = reinterpret_cast<SYS_HEADER*>(package.data());
    if (header->header_size != sizeof(SYS_HEADER) ||
        header->payload_size != package.size() - header->header_size) return nullptr;
    return reinterpret_cast<SYS_ELF64_HEADER*>(package.data() + header->header_size);
}

static void RefreshCrc(std::vector<uint8_t>& package) {
    auto* header = reinterpret_cast<SYS_HEADER*>(package.data());
    header->payload_crc32 = SysPackageCrc32(
        package.data() + header->header_size, header->payload_size);
}

static SYS_ELF64_SECTION* FindSection(
    std::vector<uint8_t>& package, const char* wanted) {
    auto* elf = PayloadElf(package);
    if (!elf || elf->shstrndx >= elf->shnum) return nullptr;
    auto* sections = reinterpret_cast<SYS_ELF64_SECTION*>(
        package.data() + sizeof(SYS_HEADER) + elf->shoff);
    const auto* namesSection = &sections[elf->shstrndx];
    const char* names = reinterpret_cast<const char*>(
        package.data() + sizeof(SYS_HEADER) + namesSection->offset);
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        if (sections[i].name >= namesSection->size) continue;
        const char* name = names + sections[i].name;
        const uint64_t available = namesSection->size - sections[i].name;
        size_t length = 0;
        while (length < available && name[length]) ++length;
        if (length == std::strlen(wanted) &&
            std::memcmp(name, wanted, length) == 0) return &sections[i];
    }
    return nullptr;
}

static SYS_ELF_LOAD_RESULT Relocate(
    const std::vector<uint8_t>& package, uint8_t* image,
    uint64_t imageCapacity, uint64_t loadAddress,
    SYS_ELF_LOAD_IMAGE* result) {
    return RelocateSysElf64Image(
        package.data(), package.size(), image, imageCapacity,
        loadAddress, result);
}

static const SYS_ELF_LOADED_SECTION* LoadedSection(
    const SYS_ELF_LOAD_IMAGE& image, uint16_t sectionIndex) {
    for (uint32_t i = 0; i < image.loaded_section_count; ++i) {
        if (image.sections[i].section_index == sectionIndex)
            return &image.sections[i];
    }
    return nullptr;
}

static const SYS_ELF64_SYMBOL* FindSymbol(
    const std::vector<uint8_t>& package, const char* wanted) {
    if (package.size() < sizeof(SYS_HEADER)) return nullptr;
    const auto* header = reinterpret_cast<const SYS_HEADER*>(package.data());
    const auto* elf = reinterpret_cast<const SYS_ELF64_HEADER*>(
        package.data() + header->header_size);
    const auto* payload = package.data() + header->header_size;
    const auto* sections = reinterpret_cast<const SYS_ELF64_SECTION*>(
        payload + elf->shoff);
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto& symtab = sections[i];
        if (symtab.type != SYS_SHT_SYMTAB || symtab.link >= elf->shnum ||
            symtab.entsize != sizeof(SYS_ELF64_SYMBOL)) continue;
        const auto& strings = sections[symtab.link];
        if (strings.type != SYS_SHT_STRTAB) continue;
        const char* names = reinterpret_cast<const char*>(payload + strings.offset);
        const auto* symbols = reinterpret_cast<const SYS_ELF64_SYMBOL*>(
            payload + symtab.offset);
        const uint64_t count = symtab.size / sizeof(SYS_ELF64_SYMBOL);
        for (uint64_t n = 0; n < count; ++n) {
            const auto& symbol = symbols[n];
            if (symbol.name >= strings.size) continue;
            const char* name = names + symbol.name;
            const uint64_t remaining = strings.size - symbol.name;
            size_t length = 0;
            while (length < remaining && name[length]) ++length;
            if (length == std::strlen(wanted) &&
                std::memcmp(name, wanted, length) == 0)
                return &symbols[n];
        }
    }
    return nullptr;
}

static bool RelocationsResolveSymbol(
    const std::vector<uint8_t>& package,
    const SYS_ELF_LOAD_IMAGE& loaded, const uint8_t* image,
    uint64_t loadAddress, const char* wanted, bool sectionSymbol) {
    if (package.size() < sizeof(SYS_HEADER)) return false;
    const auto* header = reinterpret_cast<const SYS_HEADER*>(package.data());
    const auto* payload = package.data() + header->header_size;
    const auto* elf = reinterpret_cast<const SYS_ELF64_HEADER*>(payload);
    const auto* sections = reinterpret_cast<const SYS_ELF64_SECTION*>(
        payload + elf->shoff);
    bool matched = false;
    for (uint16_t ri = 0; ri < elf->shnum; ++ri) {
        const auto& relaSection = sections[ri];
        if (relaSection.type != SYS_SHT_RELA || relaSection.info >= elf->shnum ||
            relaSection.link >= elf->shnum) continue;
        const auto* target = LoadedSection(loaded,
            static_cast<uint16_t>(relaSection.info));
        if (!target || relaSection.entsize != sizeof(SYS_ELF64_RELA)) continue;
        const auto& symtab = sections[relaSection.link];
        if (symtab.type != SYS_SHT_SYMTAB || symtab.link >= elf->shnum ||
            symtab.entsize != sizeof(SYS_ELF64_SYMBOL)) continue;
        const auto& strings = sections[symtab.link];
        const char* names = reinterpret_cast<const char*>(payload + strings.offset);
        const auto* symbols = reinterpret_cast<const SYS_ELF64_SYMBOL*>(
            payload + symtab.offset);
        const auto* relas = reinterpret_cast<const SYS_ELF64_RELA*>(
            payload + relaSection.offset);
        const uint64_t relocationCount = relaSection.size / sizeof(SYS_ELF64_RELA);
        const uint64_t symbolCount = symtab.size / sizeof(SYS_ELF64_SYMBOL);
        for (uint64_t r = 0; r < relocationCount; ++r) {
            const auto& rela = relas[r];
            const uint32_t symbolIndex = static_cast<uint32_t>(rela.info >> 32);
            if (symbolIndex >= symbolCount) continue;
            const auto& symbol = symbols[symbolIndex];
            bool nameMatches = false;
            if (sectionSymbol) {
                nameMatches = ((symbol.info & 0x0fu) == 3u) &&
                    symbol.shndx < elf->shnum;
            } else if (symbol.name < strings.size) {
                const char* name = names + symbol.name;
                const uint64_t remaining = strings.size - symbol.name;
                size_t length = 0;
                while (length < remaining && name[length]) ++length;
                nameMatches = length == std::strlen(wanted) &&
                    std::memcmp(name, wanted, length) == 0;
            }
            if (!nameMatches) continue;
            const auto* symbolSection = LoadedSection(loaded, symbol.shndx);
            if (!symbolSection || rela.offset > target->size ||
                sizeof(uint64_t) > target->size - rela.offset) return false;
            uint64_t value = loadAddress + symbolSection->image_offset + symbol.value;
            if (rela.addend >= 0) value += static_cast<uint64_t>(rela.addend);
            else value -= static_cast<uint64_t>(-(rela.addend + 1)) + 1ULL;
            const uint64_t targetOffset = target->image_offset + rela.offset;
            if (ReadU64LE(image + targetOffset) != value) return false;
            matched = true;
        }
    }
    return matched;
}

static bool LoadedImageMatchesElfMetadata(
    const std::vector<uint8_t>& package, const SYS_ELF_LOAD_IMAGE& loaded) {
    if (package.size() < sizeof(SYS_HEADER)) return false;
    const auto* header = reinterpret_cast<const SYS_HEADER*>(package.data());
    const auto* payload = package.data() + header->header_size;
    const auto* elf = reinterpret_cast<const SYS_ELF64_HEADER*>(payload);
    const auto* sections = reinterpret_cast<const SYS_ELF64_SECTION*>(
        payload + elf->shoff);
    uint32_t expectedSections = 0;
    uint32_t expectedRelocations = 0;
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto& section = sections[i];
        if (section.flags & SYS_SHF_ALLOC) ++expectedSections;
    }
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto& section = sections[i];
        if ((section.type == SYS_SHT_RELA || section.type == SYS_SHT_REL) &&
            section.info < elf->shnum &&
            (sections[section.info].flags & SYS_SHF_ALLOC)) {
            if (section.entsize == 0 || section.size % section.entsize != 0)
                return false;
            expectedRelocations += static_cast<uint32_t>(section.size / section.entsize);
        }
    }
    return expectedSections == loaded.loaded_section_count &&
        expectedRelocations == loaded.applied_relocation_count &&
        loaded.image_size > 0 && loaded.image_size <= SYS_ELF_LOAD_MAX_IMAGE_SIZE &&
        loaded.image_alignment >= 16 &&
        (loaded.image_alignment & (loaded.image_alignment - 1ULL)) == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s path/to/intlgpudvs.sys\n", argv[0]);
        return 2;
    }

    const std::vector<uint8_t> package = ReadFile(argv[1]);
    Check(!package.empty(), "live intlgpudvs.sys package read");
    if (package.empty()) return 2;
    Check(ValidateSysPackage(package.data(), package.size()) ==
        SYS_VALIDATE_RESULT::Valid, "STSYS001 package and ELF metadata validate");

    void* allocation = nullptr;
    const size_t imageCapacity = 0x4000;
    if (posix_memalign(&allocation, 4096, imageCapacity) != 0 || !allocation)
        return 3;
    auto* image = static_cast<uint8_t*>(allocation);
    std::memset(image, 0xa5, imageCapacity);
    SYS_ELF_LOAD_IMAGE loaded{};
    const uint64_t loadAddress = reinterpret_cast<uintptr_t>(image);
    const SYS_ELF_LOAD_RESULT validResult = Relocate(
        package, image, imageCapacity, loadAddress, &loaded);
    Check(validResult == SYS_ELF_LOAD_RESULT::Valid,
        "ELF alloc sections copied and R_X86_64_64 relocations applied");
    if (validResult == SYS_ELF_LOAD_RESULT::Valid) {
        Check(LoadedImageMatchesElfMetadata(package, loaded),
            "loaded image size, alignment, sections and relocation count match ELF metadata");
        Check(loaded.entry_offset < loaded.image_size &&
            loaded.entry_address == loadAddress + loaded.entry_offset,
            "entry address resolves inside relocated image");
        const auto* elf = PayloadElf(const_cast<std::vector<uint8_t>&>(package));
        const auto* textSection = FindSection(const_cast<std::vector<uint8_t>&>(package), ".ltext");
        const auto* loadedText = textSection ? LoadedSection(loaded,
            static_cast<uint16_t>(textSection - reinterpret_cast<const SYS_ELF64_SECTION*>(
                package.data() + sizeof(SYS_HEADER) + elf->shoff))) : nullptr;
        Check(textSection && loadedText && loadedText->size == textSection->size &&
            image[loadedText->image_offset] == package[sizeof(SYS_HEADER) + textSection->offset],
            "allocated executable section copied to its metadata-derived image offset");
        Check(RelocationsResolveSymbol(package, loaded, image, loadAddress,
                nullptr, true),
            "section-symbol relocation resolves using actual ELF symbol metadata");
        Check(RelocationsResolveSymbol(package, loaded, image, loadAddress,
                "StarlixGpuFillRect", false),
            "global function relocation resolves StarlixGpuFillRect");
        Check(RelocationsResolveSymbol(package, loaded, image, loadAddress,
                "StarlixGpuFrameFillRect", false),
            "frame API relocation resolves StarlixGpuFrameFillRect");
        const auto* entrySymbol = FindSymbol(package, "StarlixGpuKextEntry");
        const auto* entrySection = entrySymbol ? LoadedSection(loaded, entrySymbol->shndx) : nullptr;
        Check(entrySymbol && entrySection &&
            loaded.entry_offset == entrySection->image_offset + entrySymbol->value,
            "entry offset matches the exported KEXT entry symbol metadata");
    }

    SYS_ELF_LOAD_IMAGE ignored{};
    Check(Relocate(package, image, 1, loadAddress, &ignored) ==
        SYS_ELF_LOAD_RESULT::ImageBufferTooSmall,
        "undersized destination rejected");
    Check(Relocate(package, image, imageCapacity, loadAddress + 1, &ignored) ==
        SYS_ELF_LOAD_RESULT::ImageBaseMismatch,
        "destination/load-address mismatch rejected");

    std::vector<uint8_t> aliasPackage = package;
    auto* aliasImage = aliasPackage.data() + sizeof(SYS_HEADER);
    Check(Relocate(aliasPackage, aliasImage, 0x1000,
            reinterpret_cast<uintptr_t>(aliasImage), &ignored) ==
        SYS_ELF_LOAD_RESULT::ImageBufferOverlap,
        "destination overlapping package bytes rejected");
    Check(Relocate(package, image, imageCapacity, loadAddress,
            reinterpret_cast<SYS_ELF_LOAD_IMAGE*>(image)) ==
        SYS_ELF_LOAD_RESULT::MetadataBufferOverlap,
        "metadata overlapping destination rejected");

    void* unalignedAllocation = std::malloc(imageCapacity + 1);
    if (!unalignedAllocation) return 3;
    auto* unaligned = static_cast<uint8_t*>(unalignedAllocation) + 1;
    Check(Relocate(package, unaligned, imageCapacity,
            reinterpret_cast<uintptr_t>(unaligned), &ignored) ==
        SYS_ELF_LOAD_RESULT::ImageBaseAlignment,
        "misaligned image base rejected");
    std::free(unalignedAllocation);

    std::vector<uint8_t> badCrc = package;
    badCrc.back() ^= 0x80;
    Check(Relocate(badCrc, image, imageCapacity, loadAddress, &ignored) ==
        SYS_ELF_LOAD_RESULT::PackageInvalid,
        "corrupt payload CRC rejected");

    std::vector<uint8_t> badRelocation = package;
    auto* rela = FindSection(badRelocation, ".rela.ltext");
    if (rela) {
        auto* payload = badRelocation.data() + sizeof(SYS_HEADER);
        auto* first = reinterpret_cast<SYS_ELF64_RELA*>(payload + rela->offset);
        first->info = (first->info & 0xffffffff00000000ULL) | 2ULL;
        RefreshCrc(badRelocation);
    }
    Check(rela && Relocate(badRelocation, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::UnsupportedRelocation,
        "unsupported executable relocation type rejected");
    std::memset(image, 0x6c, imageCapacity);
    Check(rela && Relocate(badRelocation, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::UnsupportedRelocation &&
            image[0] == 0x6c && image[0x100] == 0x6c &&
            image[imageCapacity - 1] == 0x6c,
        "failed preflight leaves the caller image buffer untouched");

    std::vector<uint8_t> badTarget = package;
    rela = FindSection(badTarget, ".rela.ltext");
    auto* ltext = FindSection(badTarget, ".ltext");
    if (rela && ltext) {
        auto* payload = badTarget.data() + sizeof(SYS_HEADER);
        auto* first = reinterpret_cast<SYS_ELF64_RELA*>(payload + rela->offset);
        first->offset = ltext->size - 4;
        RefreshCrc(badTarget);
    }
    Check(rela && ltext && Relocate(badTarget, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::RelocationBounds,
        "relocation write outside target section rejected");

    std::vector<uint8_t> undefined = package;
    rela = FindSection(undefined, ".rela.ltext");
    auto* symtab = FindSection(undefined, ".symtab");
    if (rela && symtab) {
        auto* payload = undefined.data() + sizeof(SYS_HEADER);
        auto* first = reinterpret_cast<SYS_ELF64_RELA*>(payload + rela->offset);
        const uint32_t symbolIndex = static_cast<uint32_t>(first->info >> 32);
        auto* symbol = reinterpret_cast<SYS_ELF64_SYMBOL*>(
            payload + symtab->offset +
            static_cast<uint64_t>(symbolIndex) * sizeof(SYS_ELF64_SYMBOL));
        symbol->shndx = SYS_SHN_UNDEF;
        RefreshCrc(undefined);
    }
    Check(rela && symtab && Relocate(undefined, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::UndefinedSymbol,
        "undefined external relocation symbol rejected");

    std::vector<uint8_t> writableExecutable = package;
    ltext = FindSection(writableExecutable, ".ltext");
    if (ltext) {
        ltext->flags |= SYS_SHF_WRITE;
        RefreshCrc(writableExecutable);
    }
    Check(ltext && Relocate(writableExecutable, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::SectionFlags,
        "writable-executable section rejected (W^X)");

    std::vector<uint8_t> missingEntry = package;
    auto* missingHeader = reinterpret_cast<SYS_HEADER*>(missingEntry.data());
    std::memset(missingHeader->entry_symbol, 0,
        sizeof(missingHeader->entry_symbol));
    std::memcpy(missingHeader->entry_symbol,
        "StarlixGpuFrameDriverMissing", 27);
    Check(Relocate(missingEntry, image, imageCapacity,
            loadAddress, &ignored) == SYS_ELF_LOAD_RESULT::EntrySymbol,
        "missing package entry symbol rejected");

    std::free(allocation);
    if (failures) {
        std::printf("SYS_ELF_LOAD_HOST: FAIL (%d failures)\n", failures);
        return 1;
    }
    std::printf("SYS_ELF_LOAD_HOST: PASS (ELF metadata-derived relocations; no code executed)\n");
    return 0;
}
