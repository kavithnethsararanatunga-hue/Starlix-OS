// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::loader::ect {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

constexpr u32 HEADER_BYTES = 128u;
constexpr u32 SECTION_HEADER_BYTES = 24u;
constexpr u32 MAX_SECTIONS = 8u;
constexpr u64 PAGE_BYTES = 4096ULL;
constexpr u64 DEFAULT_IMAGE_BASE = 0x0000008000100000ULL;
constexpr u64 MAX_IMAGE_SPAN = 64ULL * 1024ULL * 1024ULL;

constexpr u32 SECTION_READ  = 1u << 0;
constexpr u32 SECTION_WRITE = 1u << 1;
constexpr u32 SECTION_EXEC  = 1u << 2;

struct SectionPlan {
    char name[8];
    u32 virtual_offset;
    u32 byte_size;
    u32 file_offset;
    u32 flags;
    u64 virtual_address;
};

struct LoadPlan {
    u64 image_base;
    u64 entry_point;
    u64 image_end;
    u32 section_count;
    u32 image_type;
    SectionPlan sections[MAX_SECTIONS];
};

/* Parse metadata only. No memory is allocated, mapped, or executed. */
bool validate_image(const void* bytes, u64 size, LoadPlan& out_plan);
u32 crc32(const u8* bytes, u64 size);

} // namespace starlix::loader::ect
