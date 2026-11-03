// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "ect_format.hpp"

namespace starlix::loader::ect {

constexpr u32 MAPPED_IMAGE_MAGIC = 0x324D5445u; // "ETM2"
constexpr u64 MAX_MAPPED_PAGES =
    (MAX_IMAGE_SPAN / PAGE_BYTES) + MAX_SECTIONS;

struct SectionMapping {
    u64 virtual_base;
    u64 physical_base;
    u32 byte_size;
    u32 page_count;
    u32 mapped_pages; // A mapped prefix; useful for fail-closed cleanup retries.
    u32 flags;
};

struct MappedImage {
    u32 magic;
    u32 section_count;
    u32 total_pages;
    u32 reserved;
    u64 image_base;
    u64 entry_point;
    u64 address_space_root; // Prevent a handle being used against another process root.
    SectionMapping sections[MAX_SECTIONS];
};

using AllocatePagesFn = u64 (*)(void* backend_context, u64 page_count);
using ReleasePagesFn = bool (*)(void* backend_context, u64 physical_base, u64 page_count);
using PhysicalToVirtualFn = void* (*)(void* backend_context, u64 physical_base, u64 byte_count);
using MapUserPageFn = bool (*)(void* backend_context, void* address_space_context,
                               u64 virtual_address, u64 physical_address, u32 permissions);
using UnmapUserPageFn = bool (*)(void* backend_context, void* address_space_context, u64 virtual_address);

struct MappingBackend {
    void* context;
    AllocatePagesFn allocate_pages;
    ReleasePagesFn release_pages;
    PhysicalToVirtualFn physical_to_virtual;
    MapUserPageFn map_user_page;
    UnmapUserPageFn unmap_user_page;
};

/* Validate, allocate zeroed section runs, copy payload bytes, then map pages.
   On a mapping failure every successful earlier mapping is rolled back and
   its physical run released. If rollback itself fails, out_image contains the
   residual mapped-prefix records and can be passed to unmap_image_transactional. */
bool map_image_transactional(const void* image_bytes, u64 image_size,
                              const MappingBackend& backend,
                              void* address_space_context,
                              u64 address_space_root,
                              MappedImage& out_image);

/* Unmap an inactive image and release its managed physical runs. On partial
   failure the handle retains only sections that still need cleanup. */
bool unmap_image_transactional(const MappingBackend& backend,
                               void* address_space_context,
                               u64 address_space_root,
                               MappedImage& image);

} // namespace starlix::loader::ect
