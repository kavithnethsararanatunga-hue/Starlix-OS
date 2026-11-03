// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "ect_loader.hpp"
#include "../mem/address_space.hpp"
#include "../mem/managed_page_pool.hpp"
#include "../mem/phys.hpp"

namespace starlix::loader {

void loader_init() {
    // ECT parsing is ready; execution remains disabled until memory/process
    // staging is connected through a separately validated load transaction.
}

bool StNtInspectExecutable(const void* ect_buffer, uint64_t size,
                           ect::LoadPlan* out_plan) {
    if (!out_plan) return false;
    *out_plan = ect::LoadPlan{};
    return ect::validate_image(ect_buffer, size, *out_plan);
}

namespace {

uint64_t ect_allocate_pages(void*, uint64_t page_count) {
    return starlix::mem::managed_alloc_phys_pages(page_count);
}

bool ect_release_pages(void*, uint64_t physical_base, uint64_t page_count) {
    return starlix::mem::managed_release_phys_pages(physical_base, page_count);
}

void* ect_physical_to_virtual(void*, uint64_t physical_base, uint64_t byte_count) {
    constexpr uint64_t IDENTITY_MAP_LIMIT = 0x100000000ULL;
    if (physical_base == 0ULL || byte_count == 0ULL ||
        physical_base >= IDENTITY_MAP_LIMIT ||
        byte_count > IDENTITY_MAP_LIMIT - physical_base) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(physical_base));
}

bool ect_map_user_page(void*, void* context, uint64_t virtual_address,
                       uint64_t physical_address, uint32_t permissions) {
    if (!context) return false;
    auto* space = static_cast<starlix::mem::AddressSpace*>(context);
    return starlix::mem::address_space_map_user_page(
        *space, virtual_address, physical_address, permissions);
}

bool ect_unmap_user_page(void*, void* context, uint64_t virtual_address) {
    if (!context) return false;
    auto* space = static_cast<starlix::mem::AddressSpace*>(context);
    return starlix::mem::address_space_unmap_user_page(*space, virtual_address);
}

starlix::loader::ect::MappingBackend ect_mapping_backend() {
    starlix::loader::ect::MappingBackend backend{};
    backend.context = nullptr;
    backend.allocate_pages = ect_allocate_pages;
    backend.release_pages = ect_release_pages;
    backend.physical_to_virtual = ect_physical_to_virtual;
    backend.map_user_page = ect_map_user_page;
    backend.unmap_user_page = ect_unmap_user_page;
    return backend;
}

} // namespace

bool StNtMapExecutable(const void* ect_buffer, uint64_t size,
                       starlix::mem::AddressSpace* target_space,
                       ect::MappedImage* out_mapping) {
    if (out_mapping) *out_mapping = ect::MappedImage{};
    if (!ect_buffer || !target_space || !out_mapping ||
        !starlix::mem::phys_allocator_ready() ||
        !starlix::mem::address_space_manager_ready() ||
        target_space->magic != starlix::mem::ADDRESS_SPACE_MAGIC ||
        target_space->cr3 == 0ULL ||
        !starlix::mem::address_space_root_registered(target_space->cr3) ||
        target_space->cr3 == starlix::mem::address_space_active_root()) {
        return false;
    }

    const ect::MappingBackend backend = ect_mapping_backend();
    return ect::map_image_transactional(ect_buffer, size, backend,
                                         target_space, target_space->cr3,
                                         *out_mapping);
}

bool StNtUnmapExecutable(starlix::mem::AddressSpace* target_space,
                         ect::MappedImage* mapping) {
    if (!target_space || !mapping ||
        target_space->magic != starlix::mem::ADDRESS_SPACE_MAGIC ||
        target_space->cr3 == 0ULL ||
        !starlix::mem::address_space_root_registered(target_space->cr3) ||
        target_space->cr3 == starlix::mem::address_space_active_root()) {
        return false;
    }
    const ect::MappingBackend backend = ect_mapping_backend();
    if (mapping->address_space_root != target_space->cr3) return false;
    return ect::unmap_image_transactional(backend, target_space,
                                          target_space->cr3, *mapping);
}

uint64_t StNtLoadExecutable(const void* ect_buffer, uint64_t size) {
    // Fail closed: mapping an image is not equivalent to creating a process.
    // Phase 2K exposes StNtMapExecutable separately; process/thread admission,
    // initial user stack construction, and Ring-3 launch remain disabled.
    (void)ect_buffer;
    (void)size;
    return 0ULL;
}

} // namespace starlix::loader
