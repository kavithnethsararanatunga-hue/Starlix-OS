#pragma once

#if !defined(STARLIX_PRIMITIVE_TYPES_DEFINED)
#include <stddef.h>
#include <stdint.h>
#endif
#include "sys_format.hpp"

namespace starlix::sys {

enum class SYS_VALIDATE_RESULT : uint32_t {
    Valid = 0,
    InvalidArgument,
    PackageSize,
    Magic,
    Header,
    Version,
    ReservedFields,
    Architecture,
    PayloadFormat,
    Flags,
    StringField,
    PayloadSize,
    PayloadCrc,
    ElfHeader,
    ElfIdentity,
    ElfSectionTable,
    ElfSectionBounds,
    ElfStringTable,
    ElfSymbolTable,
    ElfRelocation,
    EntrySymbol
};

constexpr uint64_t MAX_SYS_PACKAGE_SIZE = 16ULL * 1024ULL * 1024ULL;

#pragma pack(push, 1)
struct SYS_ELF64_HEADER {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct SYS_ELF64_SECTION {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct SYS_ELF64_SYMBOL {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct SYS_ELF64_REL {
    uint64_t offset;
    uint64_t info;
};

struct SYS_ELF64_RELA {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};
#pragma pack(pop)

static_assert(sizeof(SYS_HEADER) == 104, "SYS header layout changed");
static_assert(sizeof(SYS_ELF64_HEADER) == 64, "ELF64 header layout changed");
static_assert(sizeof(SYS_ELF64_SECTION) == 64, "ELF64 section layout changed");
static_assert(sizeof(SYS_ELF64_SYMBOL) == 24, "ELF64 symbol layout changed");
static_assert(sizeof(SYS_ELF64_REL) == 16, "ELF64 REL layout changed");
static_assert(sizeof(SYS_ELF64_RELA) == 24, "ELF64 RELA layout changed");

inline bool SysRangeWithin(
    uint64_t offset, uint64_t length, uint64_t total) {
    return offset <= total && length <= total - offset;
}

inline bool SysBytesEqual(
    const uint8_t* bytes, const char* text, size_t length) {
    if (!bytes || !text) return false;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != static_cast<uint8_t>(text[i])) return false;
    }
    return true;
}

inline bool SysFixedTextField(
    const char* value, size_t capacity,
    bool spacesAllowed, size_t* lengthOut) {
    if (!value || capacity == 0 || !lengthOut) return false;

    for (size_t i = 0; i < capacity; ++i) {
        const auto c = static_cast<uint8_t>(value[i]);
        if (c == 0) {
            if (i == 0) return false;
            for (size_t n = i + 1; n < capacity; ++n) {
                if (value[n] != 0) return false;
            }
            *lengthOut = i;
            return true;
        }
        if (c < (spacesAllowed ? 0x20 : 0x21) || c > 0x7e) return false;
    }
    return false;
}

inline uint32_t SysPackageCrc32(
    const uint8_t* data, uint64_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

inline bool SysElfStringEquals(
    const uint8_t* table, uint64_t tableSize,
    uint32_t offset, const char* expected, size_t expectedLength) {
    if (!table || !expected || offset >= tableSize) return false;

    uint64_t length = 0;
    while (static_cast<uint64_t>(offset) + length < tableSize &&
           table[offset + length] != 0) {
        ++length;
    }
    if (static_cast<uint64_t>(offset) + length >= tableSize) return false;
    if (length != expectedLength) return false;

    return SysBytesEqual(table + offset, expected, expectedLength);
}

inline const SYS_ELF64_SECTION* SysElfSectionAt(
    const uint8_t* payload,
    const SYS_ELF64_HEADER* elf,
    uint16_t index) {
    const uint64_t offset =
        elf->shoff + static_cast<uint64_t>(index) * elf->shentsize;
    return reinterpret_cast<const SYS_ELF64_SECTION*>(payload + offset);
}

inline SYS_VALIDATE_RESULT ValidateSysElf64Relocatable(
    const uint8_t* payload, uint64_t payloadSize,
    const char* entrySymbol, size_t entrySymbolLength) {
    if (!payload || payloadSize < sizeof(SYS_ELF64_HEADER) ||
        !entrySymbol || entrySymbolLength == 0) {
        return SYS_VALIDATE_RESULT::ElfHeader;
    }

    const auto* elf =
        reinterpret_cast<const SYS_ELF64_HEADER*>(payload);

    if (elf->ident[0] != 0x7f || elf->ident[1] != 'E' ||
        elf->ident[2] != 'L' || elf->ident[3] != 'F' ||
        elf->ident[4] != 2 || elf->ident[5] != 1 ||
        elf->ident[6] != 1 || elf->version != 1 ||
        elf->type != 1 || elf->machine != 0x3e ||
        elf->ehsize != sizeof(SYS_ELF64_HEADER)) {
        return SYS_VALIDATE_RESULT::ElfIdentity;
    }

    if (elf->shnum == 0 ||
        elf->shentsize != sizeof(SYS_ELF64_SECTION) ||
        elf->shoff > payloadSize ||
        elf->shnum > (payloadSize - elf->shoff) / elf->shentsize ||
        (elf->shstrndx != 0 && elf->shstrndx >= elf->shnum)) {
        return SYS_VALIDATE_RESULT::ElfSectionTable;
    }

    // Check every section's file bounds before examining section contents.
    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* section = SysElfSectionAt(payload, elf, i);

        if (section->offset > payloadSize) {
            return SYS_VALIDATE_RESULT::ElfSectionBounds;
        }
        if (section->type != 8 &&
            section->size > payloadSize - section->offset) {
            return SYS_VALIDATE_RESULT::ElfSectionBounds;
        }
    }

    if (elf->shstrndx != 0) {
        const auto* names = SysElfSectionAt(payload, elf, elf->shstrndx);
        if (names->type != 3 || names->size == 0) {
            return SYS_VALIDATE_RESULT::ElfStringTable;
        }

        const auto* strings = payload + names->offset;
        for (uint16_t i = 0; i < elf->shnum; ++i) {
            const auto* section = SysElfSectionAt(payload, elf, i);
            if (section->name >= names->size) {
                return SYS_VALIDATE_RESULT::ElfStringTable;
            }
            uint64_t pos = section->name;
            while (pos < names->size && strings[pos] != 0) ++pos;
            if (pos >= names->size) {
                return SYS_VALIDATE_RESULT::ElfStringTable;
            }
        }
    }

    bool entryFound = false;

    for (uint16_t i = 0; i < elf->shnum; ++i) {
        const auto* section = SysElfSectionAt(payload, elf, i);

        if (section->type == 2 || section->type == 11) {
            if (section->entsize != sizeof(SYS_ELF64_SYMBOL) ||
                section->size % sizeof(SYS_ELF64_SYMBOL) != 0 ||
                section->link >= elf->shnum) {
                return SYS_VALIDATE_RESULT::ElfSymbolTable;
            }

            const auto* stringsSection =
                SysElfSectionAt(payload, elf,
                    static_cast<uint16_t>(section->link));
            if (stringsSection->type != 3 || stringsSection->size == 0) {
                return SYS_VALIDATE_RESULT::ElfStringTable;
            }

            const auto* strings = payload + stringsSection->offset;
            for (uint64_t pos = 0; pos < section->size;
                 pos += sizeof(SYS_ELF64_SYMBOL)) {
                const auto* symbol = reinterpret_cast<const SYS_ELF64_SYMBOL*>(
                    payload + section->offset + pos);

                if (symbol->name >= stringsSection->size) {
                    return SYS_VALIDATE_RESULT::ElfSymbolTable;
                }

                uint64_t nameEnd = symbol->name;
                while (nameEnd < stringsSection->size &&
                       strings[nameEnd] != 0) ++nameEnd;
                if (nameEnd >= stringsSection->size) {
                    return SYS_VALIDATE_RESULT::ElfStringTable;
                }

                if (symbol->shndx == 0xffff ||
                    (symbol->shndx != 0 && symbol->shndx < 0xff00 &&
                     symbol->shndx >= elf->shnum)) {
                    return SYS_VALIDATE_RESULT::ElfSymbolTable;
                }

                const auto binding = symbol->info >> 4;
                const auto type = symbol->info & 0x0f;
                if (symbol->shndx != 0 && symbol->shndx < elf->shnum &&
                    (binding == 1 || binding == 2) &&
                    (type == 0 || type == 2) &&
                    SysElfStringEquals(
                        strings, stringsSection->size, symbol->name,
                        entrySymbol, entrySymbolLength)) {
                    entryFound = true;
                }
            }
        }

        if (section->type == 4 || section->type == 9) {
            const uint64_t expectedSize =
                section->type == 4
                    ? sizeof(SYS_ELF64_RELA) : sizeof(SYS_ELF64_REL);

            if (section->entsize != expectedSize ||
                section->size % expectedSize != 0 ||
                section->link >= elf->shnum ||
                section->info >= elf->shnum) {
                return SYS_VALIDATE_RESULT::ElfRelocation;
            }

            const auto* symtab = SysElfSectionAt(
                payload, elf, static_cast<uint16_t>(section->link));
            if ((symtab->type != 2 && symtab->type != 11) ||
                symtab->entsize != sizeof(SYS_ELF64_SYMBOL) ||
                symtab->size % sizeof(SYS_ELF64_SYMBOL) != 0) {
                return SYS_VALIDATE_RESULT::ElfRelocation;
            }

            const uint64_t symbolCount =
                symtab->size / sizeof(SYS_ELF64_SYMBOL);
            for (uint64_t pos = 0; pos < section->size;
                 pos += expectedSize) {
                uint64_t info = 0;
                const auto* record = payload + section->offset + pos;
                for (unsigned byte = 0; byte < 8; ++byte) {
                    info |= static_cast<uint64_t>(record[8 + byte])
                            << (byte * 8);
                }
                if ((info >> 32) >= symbolCount) {
                    return SYS_VALIDATE_RESULT::ElfRelocation;
                }
            }
        }
    }

    return entryFound
        ? SYS_VALIDATE_RESULT::Valid
        : SYS_VALIDATE_RESULT::EntrySymbol;
}

inline SYS_VALIDATE_RESULT ValidateSysPackage(
    const uint8_t* package, uint64_t packageSize) {
    if (!package) return SYS_VALIDATE_RESULT::InvalidArgument;
    if (packageSize < sizeof(SYS_HEADER) ||
        packageSize > MAX_SYS_PACKAGE_SIZE) {
        return SYS_VALIDATE_RESULT::PackageSize;
    }

    const auto* header = reinterpret_cast<const SYS_HEADER*>(package);

    if (!SysBytesEqual(
            reinterpret_cast<const uint8_t*>(header->magic),
            "STSYS001", 8)) {
        return SYS_VALIDATE_RESULT::Magic;
    }
    if (header->header_size != sizeof(SYS_HEADER)) {
        return SYS_VALIDATE_RESULT::Header;
    }
    if (header->version_major != VERSION_MAJOR ||
        header->version_minor != VERSION_MINOR) {
        return SYS_VALIDATE_RESULT::Version;
    }
    if (header->reserved0 != 0) {
        return SYS_VALIDATE_RESULT::ReservedFields;
    }
    for (size_t i = 0; i < sizeof(header->reserved); ++i) {
        if (header->reserved[i] != 0) {
            return SYS_VALIDATE_RESULT::ReservedFields;
        }
    }
    if (header->architecture != ARCH_X86_64) {
        return SYS_VALIDATE_RESULT::Architecture;
    }
    if (header->payload_format != PAYLOAD_ELF64_RELOCATABLE) {
        return SYS_VALIDATE_RESULT::PayloadFormat;
    }
    if (header->flags != FLAG_REQUIRES_RELOCATION) {
        return SYS_VALIDATE_RESULT::Flags;
    }

    size_t moduleNameLength = 0;
    size_t entrySymbolLength = 0;
    if (!SysFixedTextField(header->module_name,
                           sizeof(header->module_name), true,
                           &moduleNameLength) ||
        !SysFixedTextField(header->entry_symbol,
                           sizeof(header->entry_symbol), false,
                           &entrySymbolLength)) {
        return SYS_VALIDATE_RESULT::StringField;
    }
    (void)moduleNameLength;

    if (header->payload_size != packageSize - header->header_size) {
        return SYS_VALIDATE_RESULT::PayloadSize;
    }

    const auto* payload = package + header->header_size;
    if (SysPackageCrc32(payload, header->payload_size) !=
        header->payload_crc32) {
        return SYS_VALIDATE_RESULT::PayloadCrc;
    }

    return ValidateSysElf64Relocatable(
        payload, header->payload_size,
        header->entry_symbol, entrySymbolLength);
}

} // namespace starlix::sys
