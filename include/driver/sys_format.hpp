#pragma once
#if !defined(STARLIX_PRIMITIVE_TYPES_DEFINED)
#include <stdint.h>
#endif

namespace starlix::sys {

constexpr uint16_t VERSION_MAJOR = 1;
constexpr uint16_t VERSION_MINOR = 0;
constexpr uint32_t ARCH_X86_64 = 0x3E;
constexpr uint32_t PAYLOAD_ELF64_RELOCATABLE = 1;
constexpr uint32_t FLAG_REQUIRES_RELOCATION = 1u << 0;

#pragma pack(push, 1)
struct SYS_HEADER {
    char magic[8];                 // "STSYS001"
    uint16_t header_size;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t reserved0;
    uint32_t architecture;
    uint32_t payload_format;
    uint64_t payload_size;
    uint32_t payload_crc32;
    uint32_t flags;
    char module_name[24];
    char entry_symbol[32];
    uint8_t reserved[8];
};
#pragma pack(pop)

static_assert(sizeof(SYS_HEADER) == 104,
              "Starlix SYS header must be exactly 104 bytes");

} // namespace starlix::sys
