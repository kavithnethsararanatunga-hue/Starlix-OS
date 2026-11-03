// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "ect_format.hpp"
#include "../mem/user_address.hpp"

namespace starlix::loader::ect {
namespace {

u16 read_u16(const u8* p) {
    return static_cast<u16>(p[0]) |
           (static_cast<u16>(p[1]) << 8);
}

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u64 read_u64(const u8* p) {
    return static_cast<u64>(read_u32(p)) |
           (static_cast<u64>(read_u32(p + 4)) << 32);
}

bool bytes_equal(const u8* actual, const char* expected,
                 u32 expected_length, u32 field_length) {
    if (!actual || !expected || expected_length > field_length) return false;
    for (u32 i = 0; i < expected_length; ++i)
        if (actual[i] != static_cast<u8>(expected[i])) return false;
    for (u32 i = expected_length; i < field_length; ++i)
        if (actual[i] != 0u) return false;
    return true;
}

bool parse_name(const u8* raw, char (&out)[8]) {
    if (!raw) return false;
    u32 length = 0u;
    while (length < 8u && raw[length] != 0u) ++length;
    if (length == 0u || length == 8u) return false;
    for (u32 i = length; i < 8u; ++i)
        if (raw[i] != 0u) return false;
    for (u32 i = 0; i < 8u; ++i)
        out[i] = static_cast<char>(raw[i]);
    return true;
}

bool name_is(const char (&name)[8], const char* expected) {
    u32 i = 0u;
    while (i < 8u && expected[i] != '\0') {
        if (name[i] != expected[i]) return false;
        ++i;
    }
    if (i == 8u || expected[i] != '\0' || name[i] != '\0') return false;
    for (++i; i < 8u; ++i)
        if (name[i] != '\0') return false;
    return true;
}

bool add_u64(u64 a, u64 b, u64& out) {
    if (b > (~0ULL - a)) return false;
    out = a + b;
    return true;
}

bool range_within(u64 offset, u64 length, u64 total) {
    return offset <= total && length <= total - offset;
}

} // namespace

u32 crc32(const u8* bytes, u64 size) {
    if (!bytes && size != 0ULL) return 0u;
    u32 value = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (u32 bit = 0; bit < 8u; ++bit) {
            const u32 mask = 0u - (value & 1u);
            value = (value >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return value ^ 0xFFFFFFFFu;
}

bool validate_image(const void* raw_bytes, u64 size, LoadPlan& out_plan) {
    out_plan = LoadPlan{};
    if (!raw_bytes || size < HEADER_BYTES) return false;
    if (size > (MAX_IMAGE_SPAN * 2ULL)) return false;

    LoadPlan candidate{};
    const auto* bytes = static_cast<const u8*>(raw_bytes);
    if (bytes[0] != 'P' || bytes[1] != 'x' ||
        !bytes_equal(bytes + 2, "Starlix ect @PX", 15u, 32u) ||
        !bytes_equal(bytes + 34, "Starlix Native Executable Pack", 30u, 40u))
        return false;

    const u16 major = read_u16(bytes + 74);
    const u16 minor = read_u16(bytes + 76);
    const u8 image_type = bytes[78];
    const u8 architecture = bytes[79];
    const u16 header_bytes = read_u16(bytes + 80);
    const u32 section_count = read_u32(bytes + 82);
    const u32 section_offset = read_u32(bytes + 86);
    const u32 import_count = read_u32(bytes + 90);
    const u32 import_offset = read_u32(bytes + 94);
    const u32 relocation_count = read_u32(bytes + 98);
    const u32 relocation_offset = read_u32(bytes + 102);
    const u64 entry_offset = read_u64(bytes + 106);
    const u64 preferred_base = read_u64(bytes + 114);
    const u32 expected_text_crc = read_u32(bytes + 122);

    if (major != 1u || minor != 0u || image_type != 1u || architecture != 1u ||
        header_bytes != HEADER_BYTES || section_count == 0u ||
        section_count > MAX_SECTIONS || section_offset != HEADER_BYTES ||
        import_count != 0u || relocation_count != 0u ||
        preferred_base != DEFAULT_IMAGE_BASE ||
        bytes[126] != 0u || bytes[127] != 0u) {
        return false;
    }

    const u64 table_bytes =
        static_cast<u64>(section_count) * SECTION_HEADER_BYTES;
    u64 table_end = 0ULL;
    if (!add_u64(section_offset, table_bytes, table_end) ||
        !range_within(section_offset, table_bytes, size) ||
        import_offset != table_end || relocation_offset != import_offset) {
        return false;
    }

    candidate.image_base = preferred_base;
    candidate.image_type = image_type;
    candidate.section_count = section_count;
    bool found_text = false;
    u64 max_file_end = table_end;
    u64 min_file_offset = size;
    u64 total_payload_bytes = 0ULL;

    for (u32 i = 0; i < section_count; ++i) {
        const u8* sh = bytes + section_offset +
            static_cast<u64>(i) * SECTION_HEADER_BYTES;
        SectionPlan& section = candidate.sections[i];
        if (!parse_name(sh, section.name)) return false;
        section.virtual_offset = read_u32(sh + 8);
        section.byte_size = read_u32(sh + 12);
        section.file_offset = read_u32(sh + 16);
        section.flags = read_u32(sh + 20);

        if (section.byte_size == 0u ||
            (section.virtual_offset & (PAGE_BYTES - 1ULL)) != 0ULL ||
            section.virtual_offset == 0u ||
            (section.flags & ~0x7u) != 0u ||
            (section.flags & SECTION_READ) == 0u ||
            ((section.flags & SECTION_WRITE) != 0u &&
             (section.flags & SECTION_EXEC) != 0u) ||
            !range_within(section.file_offset, section.byte_size, size) ||
            section.file_offset < table_end) {
            return false;
        }

        const bool is_text = name_is(section.name, ".text");
        const bool is_rodata = name_is(section.name, ".rodata");
        const bool is_data = name_is(section.name, ".data");
        if ((!is_text && !is_rodata && !is_data) ||
            (is_text && section.flags != (SECTION_READ | SECTION_EXEC)) ||
            (is_rodata && section.flags != SECTION_READ) ||
            (is_data && section.flags != (SECTION_READ | SECTION_WRITE))) {
            return false;
        }
        for (u32 prior = 0; prior < i; ++prior)
            if (name_is(section.name, candidate.sections[prior].name)) return false;

        const u64 relative_end =
            static_cast<u64>(section.virtual_offset) + section.byte_size;
        if (relative_end > MAX_IMAGE_SPAN) return false;
        if (!add_u64(preferred_base, section.virtual_offset,
                     section.virtual_address)) return false;
        u64 absolute_end = 0ULL;
        if (!add_u64(section.virtual_address, section.byte_size, absolute_end) ||
            !starlix::mem::user_address::is_user_address(section.virtual_address) ||
            absolute_end <= section.virtual_address ||
            !starlix::mem::user_address::is_user_address(absolute_end - 1ULL)) {
            return false;
        }
        if (absolute_end > candidate.image_end) candidate.image_end = absolute_end;

        const u64 file_end =
            static_cast<u64>(section.file_offset) + section.byte_size;
        if (!add_u64(total_payload_bytes, section.byte_size,
                     total_payload_bytes)) return false;
        if (section.file_offset < min_file_offset)
            min_file_offset = section.file_offset;
        if (file_end > max_file_end) max_file_end = file_end;
        if (is_text) {
            if (found_text) return false;
            found_text = true;
            if (!(entry_offset >= section.virtual_offset &&
                  entry_offset < relative_end)) return false;
            if (crc32(bytes + section.file_offset, section.byte_size) !=
                expected_text_crc) return false;
        }
    }

    if (!found_text || min_file_offset != table_end || max_file_end != size ||
        total_payload_bytes != size - table_end)
        return false;

    for (u32 i = 0; i < section_count; ++i) {
        const SectionPlan& a = candidate.sections[i];
        const u64 a_virtual_end =
            static_cast<u64>(a.virtual_offset) + a.byte_size;
        const u64 a_file_end =
            static_cast<u64>(a.file_offset) + a.byte_size;
        for (u32 j = i + 1u; j < section_count; ++j) {
            const SectionPlan& b = candidate.sections[j];
            const u64 b_virtual_end =
                static_cast<u64>(b.virtual_offset) + b.byte_size;
            const u64 b_file_end =
                static_cast<u64>(b.file_offset) + b.byte_size;
            if (a.virtual_offset < b_virtual_end &&
                b.virtual_offset < a_virtual_end) return false;
            if (a.file_offset < b_file_end && b.file_offset < a_file_end)
                return false;
        }
    }

    if (!add_u64(preferred_base, entry_offset, candidate.entry_point) ||
        !starlix::mem::user_address::is_user_address(candidate.entry_point)) {
        out_plan = LoadPlan{};
        return false;
    }
    out_plan = candidate;
    return true;
}

} // namespace starlix::loader::ect
