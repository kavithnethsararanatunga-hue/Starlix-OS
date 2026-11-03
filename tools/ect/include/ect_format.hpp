// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include <cstdint>
#include <cstring>

namespace starlix::ect {

#pragma pack(push, 1)

constexpr char MAGIC0 = 'P';
constexpr char MAGIC1 = 'x';
constexpr uint16_t HEADER_SIZE = 128;
constexpr char BANNER[]      = "Starlix ect @PX";
constexpr char FORMAT_NAME[] = "Starlix Native Executable Pack";

enum SectionFlags : uint32_t {
    SEC_READ    = 0x1,
    SEC_WRITE   = 0x2,
    SEC_EXEC    = 0x4
};

enum RelocType : uint32_t {
    RELOC_ABSOLUTE64 = 1, // 64-bit address needs ASLR base delta added
    RELOC_RELATIVE32 = 2, // 32-bit relative displacement
    RELOC_IAT_PATCH  = 3  // Dynamic library import slot
};

struct SECTION_HEADER {
    char     name[8];          // ".text", ".data", ".rodata", ".bss"
    uint32_t virtual_addr;     // Preferred virtual offset
    uint32_t raw_size;         // Size in binary file
    uint32_t raw_offset;       // Offset in binary file
    uint32_t flags;            // SEC_READ | SEC_WRITE | SEC_EXEC
};

struct IMPORT_SYMBOL {
    char library_name[32];     // e.g. "libmath.lbr"
    char symbol_name[32];      // e.g. "starlix_add"
    uint32_t iat_offset;       // Offset in .data where pointer will be patched
};

struct RELOCATION_ENTRY {
    uint32_t virtual_addr;     // Target address offset inside section
    uint32_t type;             // RelocType
    uint32_t section_idx;      // Index of section containing the address
    uint32_t addend;           // Constant offset
};

struct ECT_HEADER {
    char     magic[2];           // 'P','x' (2)
    char     banner[32];         // "Starlix ect @PX" (32)
    char     format_name[40];    // "Starlix Native Executable Pack" (40)
    uint16_t version_major;      // 1 (2)
    uint16_t version_minor;      // 0 (2)
    uint8_t  image_type;         // 1 = Executable (1)
    uint8_t  architecture;       // 1 = x86_64 (1)
    uint16_t header_size;        // 128 (2)
    uint32_t section_count;      // (4)
    uint32_t section_table_off;  // (4)
    uint32_t import_count;       // (4)
    uint32_t import_table_off;   // (4)
    uint32_t reloc_count;        // (4)
    uint32_t reloc_table_off;    // (4)
    uint64_t entry_point;        // (8)
    uint64_t preferred_base;     // (8)
    uint32_t checksum;           // (4)
    uint8_t  reserved[2];        // Padding to exactly 128 bytes (2)
};

#pragma pack(pop)

static_assert(sizeof(ECT_HEADER) == 128, "ECT_HEADER size must be exactly 128 bytes");

inline void init_header(ECT_HEADER& h) {
    std::memset(&h, 0, sizeof(h));
    h.magic[0] = MAGIC0;
    h.magic[1] = MAGIC1;
    std::memcpy(h.banner, BANNER, sizeof(BANNER));
    std::memcpy(h.format_name, FORMAT_NAME, sizeof(FORMAT_NAME));
    h.version_major = 1;
    h.version_minor = 0;
    h.image_type = 1;
    h.architecture = 1;
    h.header_size = HEADER_SIZE;
    h.preferred_base = 0x10000;
}

inline bool validate_header(const ECT_HEADER& h, const char** reason = nullptr) {
    auto fail = [&](const char* r) { if (reason) *reason = r; return false; };
    if (h.magic[0] != MAGIC0 || h.magic[1] != MAGIC1) return fail("bad magic");
    if (std::strncmp(h.banner, BANNER, sizeof(h.banner)) != 0) return fail("bad banner");
    if (h.header_size != HEADER_SIZE) return fail("bad header size");
    return true;
}

} // namespace starlix::ect
