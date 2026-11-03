// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include <cstdint>
#include <cstring>

namespace starlix::lbr {

#pragma pack(push, 1)

constexpr char MAGIC0 = 'L';
constexpr char MAGIC1 = 'b';
constexpr char BANNER[]      = "Starlix lbr @LB";
constexpr char FORMAT_NAME[] = "Starlix Native Shared Library Pack";

enum Architecture : uint8_t { ARCH_X86_64 = 1 };

struct EXPORT_SYMBOL {
    char name[64];             // Function symbol name
    uint64_t code_offset;      // Virtual code offset inside library
};
using EXPORT_ENTRY = EXPORT_SYMBOL;

struct LBR_HEADER {
    char     magic[2];         // 'L','b'
    char     banner[32];       // "Starlix lbr @LB"
    char     format_name[64];  // "Starlix Native Shared Library Pack"
    uint16_t version_major;    // 1
    uint16_t version_minor;    // 0
    uint8_t  architecture;     // 1 = x86_64
    uint8_t  has_signature;    // 1 = Signed
    uint16_t header_size;

    uint32_t export_count;        // Number of exported functions
    uint64_t export_table_offset; // File offset of Export Table

    uint32_t import_count;
    uint64_t import_table_offset;

    uint32_t dependency_count;
    uint64_t dependency_table_offset;

    uint32_t relocation_count;
    uint64_t relocation_table_offset;

    uint64_t code_offset;      // Code offset
    uint64_t code_size;        // Code size
    uint64_t preferred_base;   // Preferred load base
    uint32_t checksum;
    uint64_t signature_offset;
    uint32_t signature_size;
    uint8_t  reserved[16];
};

#pragma pack(pop)

inline void init_header(LBR_HEADER& h) {
    std::memset(&h, 0, sizeof(h));
    h.magic[0] = MAGIC0;
    h.magic[1] = MAGIC1;
    std::memcpy(h.banner, BANNER, sizeof(BANNER));
    std::memcpy(h.format_name, FORMAT_NAME, sizeof(FORMAT_NAME));
    h.version_major = 1;
    h.version_minor = 0;
    h.architecture = ARCH_X86_64;
    h.header_size = sizeof(LBR_HEADER);
    h.preferred_base = 0x20000;
}

inline bool validate_header(const LBR_HEADER& h, const char** reason = nullptr) {
    auto fail = [&](const char* r) { if (reason) *reason = r; return false; };
    if (h.magic[0] != MAGIC0 || h.magic[1] != MAGIC1) return fail("bad magic");
    if (std::strncmp(h.banner, BANNER, sizeof(h.banner)) != 0) return fail("bad banner");
    if (h.header_size != sizeof(LBR_HEADER)) return fail("bad header size");
    return true;
}

} // namespace starlix::lbr
