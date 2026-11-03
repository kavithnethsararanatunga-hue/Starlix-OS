// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "ect_memory_map.hpp"

namespace starlix::loader::ect {
namespace {

u64 pages_for_bytes(u64 bytes) {
    if (bytes == 0ULL || bytes > (~0ULL - (PAGE_BYTES - 1ULL))) return 0ULL;
    return (bytes + PAGE_BYTES - 1ULL) / PAGE_BYTES;
}

void zero_bytes(void* raw, u64 count) {
    auto* bytes = static_cast<u8*>(raw);
    for (u64 i = 0; i < count; ++i) bytes[i] = 0u;
}

void copy_bytes(void* raw_destination, const void* raw_source, u64 count) {
    auto* destination = static_cast<u8*>(raw_destination);
    const auto* source = static_cast<const u8*>(raw_source);
    for (u64 i = 0; i < count; ++i) destination[i] = source[i];
}

bool backend_valid(const MappingBackend& backend) {
    return backend.allocate_pages && backend.release_pages &&
           backend.physical_to_virtual && backend.map_user_page &&
           backend.unmap_user_page;
}

/* Remove mapped prefixes in reverse order. Never release a physical run while
   any page from that run might still be reachable through a user PTE. */
bool rollback(MappedImage& image, const MappingBackend& backend,
              void* address_space_context) {
    bool complete = true;
    for (u32 section_index = image.section_count; section_index > 0u;
         --section_index) {
        SectionMapping& section = image.sections[section_index - 1u];
        while (section.mapped_pages != 0u) {
            const u32 page_index = section.mapped_pages - 1u;
            const u64 virtual_address = section.virtual_base +
                static_cast<u64>(page_index) * PAGE_BYTES;
            if (!backend.unmap_user_page(backend.context, address_space_context, virtual_address)) {
                complete = false;
                break;
            }
            --section.mapped_pages;
        }
        if (section.mapped_pages == 0u && section.physical_base != 0ULL) {
            if (backend.release_pages(backend.context, section.physical_base, section.page_count)) {
                section.physical_base = 0ULL;
                section.page_count = 0u;
            } else {
                complete = false;
            }
        } else if (section.physical_base != 0ULL) {
            complete = false;
        }
    }
    return complete;
}

} // namespace

bool map_image_transactional(const void* image_bytes, u64 image_size,
                              const MappingBackend& backend,
                              void* address_space_context,
                              u64 address_space_root,
                              MappedImage& out_image) {
    out_image = MappedImage{};
    if (!image_bytes || address_space_root == 0ULL ||
        !backend_valid(backend)) return false;

    LoadPlan plan{};
    if (!validate_image(image_bytes, image_size, plan)) return false;

    MappedImage candidate{};
    candidate.magic = MAPPED_IMAGE_MAGIC;
    candidate.image_base = plan.image_base;
    candidate.entry_point = plan.entry_point;
    candidate.address_space_root = address_space_root;
    candidate.section_count = plan.section_count;

    const auto* source = static_cast<const u8*>(image_bytes);
    bool success = true;

    for (u32 section_index = 0; section_index < plan.section_count;
         ++section_index) {
        const SectionPlan& source_section = plan.sections[section_index];
        SectionMapping& target = candidate.sections[section_index];
        const u64 page_count64 = pages_for_bytes(source_section.byte_size);
        if (page_count64 == 0ULL || page_count64 > 0xFFFFFFFFULL ||
            page_count64 > (MAX_MAPPED_PAGES - candidate.total_pages)) {
            success = false;
            break;
        }

        const u64 allocation_bytes = page_count64 * PAGE_BYTES;
        const u64 physical_base = backend.allocate_pages(backend.context, page_count64);
        if (physical_base == 0ULL || (physical_base & (PAGE_BYTES - 1ULL)) != 0ULL) {
            success = false;
            break;
        }

        target.virtual_base = source_section.virtual_address;
        target.physical_base = physical_base;
        target.byte_size = source_section.byte_size;
        target.page_count = static_cast<u32>(page_count64);
        target.flags = source_section.flags;
        candidate.total_pages += static_cast<u32>(page_count64);

        void* destination = backend.physical_to_virtual(backend.context, physical_base,
                                                        allocation_bytes);
        if (!destination) {
            (void)backend.release_pages(backend.context, physical_base, page_count64);
            target.physical_base = 0ULL;
            target.page_count = 0u;
            candidate.total_pages -= static_cast<u32>(page_count64);
            success = false;
            break;
        }

        zero_bytes(destination, allocation_bytes);
        copy_bytes(destination, source + source_section.file_offset,
                   source_section.byte_size);

        for (u32 page_index = 0; page_index < target.page_count; ++page_index) {
            const u64 virtual_address = target.virtual_base +
                static_cast<u64>(page_index) * PAGE_BYTES;
            const u64 physical_address = physical_base +
                static_cast<u64>(page_index) * PAGE_BYTES;
            if (!backend.map_user_page(backend.context, address_space_context,
                                       virtual_address, physical_address, target.flags)) {
                success = false;
                break;
            }
            ++target.mapped_pages;
        }
        if (!success) break;
    }

    if (!success) {
        const bool cleaned = rollback(candidate, backend, address_space_context);
        if (!cleaned) out_image = candidate; // Residual mappings remain owned by this handle.
        return false;
    }

    out_image = candidate;
    return true;
}

bool unmap_image_transactional(const MappingBackend& backend,
                               void* address_space_context,
                               u64 address_space_root,
                               MappedImage& image) {
    if (!backend_valid(backend) || image.magic != MAPPED_IMAGE_MAGIC ||
        image.section_count == 0u || image.section_count > MAX_SECTIONS ||
        address_space_root == 0ULL ||
        image.address_space_root != address_space_root) return false;

    if (!rollback(image, backend, address_space_context)) return false;
    image = MappedImage{};
    return true;
}

} // namespace starlix::loader::ect
