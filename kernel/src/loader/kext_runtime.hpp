// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#if !defined(STARLIX_PRIMITIVE_TYPES_DEFINED)
#include <stddef.h>
#include <stdint.h>
#endif

#include "../../../include/driver/sys_elf_loader.hpp"
#include "../mem/address_space.hpp"
#include "../mem/managed_page_pool.hpp"

namespace starlix::loader {

constexpr uint64_t KEXT_RUNTIME_PAGE_SIZE = 4096ULL;
constexpr uint64_t KEXT_RUNTIME_IMAGE_CAPACITY = 64ULL * 1024ULL;
constexpr uint32_t KEXT_RUNTIME_PAGE_COUNT =
    static_cast<uint32_t>(KEXT_RUNTIME_IMAGE_CAPACITY / KEXT_RUNTIME_PAGE_SIZE);
constexpr uint64_t KEXT_RUNTIME_MAX_PACKAGE_SIZE = 1024ULL * 1024ULL;
constexpr uint32_t KEXT_RUNTIME_PAGE_UNMAPPED = 0xFFFFFFFFu;

// These callbacks keep the loader independent of a specific driver or device.
struct KEXT_RUNTIME_PLATFORM {
    void* context;
    uint64_t (*allocate_pages)(void*, uint64_t);
    bool (*release_pages)(void*, uint64_t, uint64_t);
    bool (*set_identity_permissions)(void*, uint64_t, uint32_t, uint32_t);
    bool (*map_module_page)(void*, uint64_t, uint64_t, uint32_t);
    bool (*unmap_module_page)(void*, uint64_t, uint64_t, uint32_t);
    bool (*guard_unmapped)(void*, uint64_t);
    uint8_t* (*resolve_write_alias)(void*, uint64_t, uint64_t);
};

enum class KEXT_RUNTIME_STATUS : uint32_t {
    Ready = 0,
    InvalidArgument,
    PackageRejected,
    MemoryUnavailable,
    MemoryInvalid,
    IdentityProtectionFailed,
    GuardPageConflict,
    ModuleMappingFailed,
    WritableAliasUnavailable,
    RelocationRejected,
    ImageReportRejected,
    PagePermissionConflict,
    EntryNotExecutable,
    RollbackFailed,
    UnloadFailed
};

struct KEXT_RUNTIME_MODULE {
    uint64_t physical_base;
    uint64_t virtual_base;
    uint64_t image_size;
    uint64_t entry_address;
    uint32_t page_count;
    uint32_t loaded_section_count;
    uint32_t applied_relocation_count;
    uint32_t state; // 0=empty, 1=loading, 2=active, 3=rollback/unload pending
    uint32_t mapped_flags[KEXT_RUNTIME_PAGE_COUNT];
    uint32_t identity_readonly_mask;
};

inline bool KextRuntimePlatformValid(const KEXT_RUNTIME_PLATFORM* platform) {
    return platform && platform->allocate_pages && platform->release_pages &&
        platform->set_identity_permissions && platform->map_module_page &&
        platform->unmap_module_page && platform->guard_unmapped &&
        platform->resolve_write_alias;
}

inline bool KextRuntimeRollback(KEXT_RUNTIME_MODULE* module,
                                const KEXT_RUNTIME_PLATFORM* platform) {
    if (!module || !platform || module->physical_base == 0ULL ||
        module->page_count != KEXT_RUNTIME_PAGE_COUNT) return false;

    // Remove every module alias before restoring the writable identity alias.
    for (uint32_t page = 0u; page < module->page_count; ++page) {
        const uint32_t flags = module->mapped_flags[page];
        if (flags == KEXT_RUNTIME_PAGE_UNMAPPED) continue;
        const uint64_t va = module->virtual_base +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        const uint64_t pa = module->physical_base +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        if (!platform->unmap_module_page(platform->context, va, pa, flags)) {
            module->state = 3u;
            return false;
        }
        module->mapped_flags[page] = KEXT_RUNTIME_PAGE_UNMAPPED;
    }

    for (uint32_t page = 0u; page < module->page_count; ++page) {
        const uint32_t bit = 1u << page;
        if ((module->identity_readonly_mask & bit) == 0u) continue;
        const uint64_t pa = module->physical_base +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        if (!platform->set_identity_permissions(platform->context, pa,
                0u, starlix::mem::KERNEL_MAPPING_WRITE)) {
            module->state = 3u;
            return false;
        }
        module->identity_readonly_mask &= ~bit;
    }

    volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(
        static_cast<uintptr_t>(module->physical_base));
    for (uint64_t i = 0u; i < KEXT_RUNTIME_IMAGE_CAPACITY; ++i) bytes[i] = 0u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint64_t i = 0u; i < KEXT_RUNTIME_IMAGE_CAPACITY; ++i) {
        if (bytes[i] != 0u) {
            module->state = 3u;
            return false;
        }
    }
    if (!platform->release_pages(platform->context, module->physical_base,
            module->page_count)) {
        module->state = 3u;
        return false;
    }
    *module = KEXT_RUNTIME_MODULE{};
    return true;
}

inline KEXT_RUNTIME_STATUS KextRuntimeLoadExternal(
    const uint8_t* package, uint64_t package_size,
    uint64_t module_virtual_base,
    const KEXT_RUNTIME_PLATFORM* platform,
    KEXT_RUNTIME_MODULE* output) {
    if (output) *output = KEXT_RUNTIME_MODULE{};
    if (!package || !output || !KextRuntimePlatformValid(platform) ||
        package_size < sizeof(starlix::sys::SYS_HEADER) ||
        package_size > KEXT_RUNTIME_MAX_PACKAGE_SIZE ||
        module_virtual_base == 0ULL ||
        (module_virtual_base & (KEXT_RUNTIME_PAGE_SIZE - 1ULL)) != 0ULL ||
        module_virtual_base < KEXT_RUNTIME_PAGE_SIZE ||
        module_virtual_base > (~0ULL - KEXT_RUNTIME_IMAGE_CAPACITY -
                               KEXT_RUNTIME_PAGE_SIZE))
        return KEXT_RUNTIME_STATUS::InvalidArgument;

    if (starlix::sys::ValidateSysPackage(package, package_size) !=
        starlix::sys::SYS_VALIDATE_RESULT::Valid)
        return KEXT_RUNTIME_STATUS::PackageRejected;

    const uint64_t lower_guard = module_virtual_base - KEXT_RUNTIME_PAGE_SIZE;
    const uint64_t upper_guard = module_virtual_base + KEXT_RUNTIME_IMAGE_CAPACITY;
    if (!platform->guard_unmapped(platform->context, lower_guard) ||
        !platform->guard_unmapped(platform->context, upper_guard))
        return KEXT_RUNTIME_STATUS::GuardPageConflict;

    const uint64_t physical = platform->allocate_pages(
        platform->context, KEXT_RUNTIME_PAGE_COUNT);
    if (physical == 0ULL) return KEXT_RUNTIME_STATUS::MemoryUnavailable;
    if ((physical & (KEXT_RUNTIME_PAGE_SIZE - 1ULL)) != 0ULL ||
        physical >= 0x100000000ULL ||
        KEXT_RUNTIME_IMAGE_CAPACITY > 0x100000000ULL - physical) {
        if ((physical & (KEXT_RUNTIME_PAGE_SIZE - 1ULL)) == 0ULL)
            (void)platform->release_pages(platform->context, physical,
                KEXT_RUNTIME_PAGE_COUNT);
        return KEXT_RUNTIME_STATUS::MemoryInvalid;
    }

    *output = KEXT_RUNTIME_MODULE{};
    output->physical_base = physical;
    output->virtual_base = module_virtual_base;
    output->page_count = KEXT_RUNTIME_PAGE_COUNT;
    output->state = 1u;
    for (uint32_t page = 0u; page < KEXT_RUNTIME_PAGE_COUNT; ++page)
        output->mapped_flags[page] = KEXT_RUNTIME_PAGE_UNMAPPED;

    // The physical identity alias is changed to RO+NX before a second alias is
    // installed. The external image is written only through the temporary
    // supervisor RW+NX module mapping.
    for (uint32_t page = 0u; page < KEXT_RUNTIME_PAGE_COUNT; ++page) {
        const uint64_t pa = physical +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        if (!platform->set_identity_permissions(platform->context, pa,
                starlix::mem::KERNEL_MAPPING_WRITE, 0u)) {
            return KextRuntimeRollback(output, platform)
                ? KEXT_RUNTIME_STATUS::IdentityProtectionFailed
                : KEXT_RUNTIME_STATUS::RollbackFailed;
        }
        output->identity_readonly_mask |= (1u << page);
    }

    for (uint32_t page = 0u; page < KEXT_RUNTIME_PAGE_COUNT; ++page) {
        const uint64_t va = module_virtual_base +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        const uint64_t pa = physical +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        if (!platform->map_module_page(platform->context, va, pa,
                starlix::mem::KERNEL_MODULE_MAP_WRITE)) {
            return KextRuntimeRollback(output, platform)
                ? KEXT_RUNTIME_STATUS::ModuleMappingFailed
                : KEXT_RUNTIME_STATUS::RollbackFailed;
        }
        output->mapped_flags[page] = 1u;
    }

    uint8_t* writable_image = platform->resolve_write_alias(
        platform->context, module_virtual_base, KEXT_RUNTIME_IMAGE_CAPACITY);
    if (!writable_image || reinterpret_cast<uint64_t>(writable_image) !=
            module_virtual_base) {
        return KextRuntimeRollback(output, platform)
            ? KEXT_RUNTIME_STATUS::WritableAliasUnavailable
            : KEXT_RUNTIME_STATUS::RollbackFailed;
    }

    starlix::sys::SYS_ELF_LOAD_IMAGE image_info{};
    const auto relocation_result = starlix::sys::RelocateSysElf64Image(
        package, package_size, writable_image, KEXT_RUNTIME_IMAGE_CAPACITY,
        module_virtual_base, &image_info);
    if (relocation_result != starlix::sys::SYS_ELF_LOAD_RESULT::Valid ||
        image_info.image_size == 0ULL ||
        image_info.image_size > KEXT_RUNTIME_IMAGE_CAPACITY ||
        image_info.entry_offset >= image_info.image_size ||
        image_info.entry_address != module_virtual_base + image_info.entry_offset ||
        image_info.loaded_section_count == 0u ||
        image_info.applied_relocation_count == 0u) {
        return KextRuntimeRollback(output, platform)
            ? KEXT_RUNTIME_STATUS::RelocationRejected
            : KEXT_RUNTIME_STATUS::RollbackFailed;
    }

    bool page_writable[KEXT_RUNTIME_PAGE_COUNT]{};
    bool page_executable[KEXT_RUNTIME_PAGE_COUNT]{};
    const uint64_t used_pages =
        (image_info.image_size + KEXT_RUNTIME_PAGE_SIZE - 1ULL) /
        KEXT_RUNTIME_PAGE_SIZE;
    if (used_pages == 0ULL || used_pages > KEXT_RUNTIME_PAGE_COUNT) {
        return KextRuntimeRollback(output, platform)
            ? KEXT_RUNTIME_STATUS::ImageReportRejected
            : KEXT_RUNTIME_STATUS::RollbackFailed;
    }

    bool section_layout_valid = true;
    for (uint32_t section_index = 0u;
         section_index < image_info.loaded_section_count; ++section_index) {
        const auto& section = image_info.sections[section_index];
        if (section.size == 0ULL) continue;
        if (section.image_offset >= image_info.image_size ||
            section.size > image_info.image_size - section.image_offset) {
            section_layout_valid = false;
            break;
        }
        const bool writable =
            (section.flags & starlix::sys::SYS_SHF_WRITE) != 0ULL;
        const bool executable =
            (section.flags & starlix::sys::SYS_SHF_EXECINSTR) != 0ULL;
        if (writable && executable) {
            section_layout_valid = false;
            break;
        }
        const uint64_t first_page = section.image_offset / KEXT_RUNTIME_PAGE_SIZE;
        const uint64_t last_page =
            (section.image_offset + section.size - 1ULL) / KEXT_RUNTIME_PAGE_SIZE;
        if (last_page >= used_pages) {
            section_layout_valid = false;
            break;
        }
        for (uint64_t page = first_page; page <= last_page; ++page) {
            if ((writable && page_executable[page]) ||
                (executable && page_writable[page])) {
                section_layout_valid = false;
                break;
            }
            page_writable[page] = page_writable[page] || writable;
            page_executable[page] = page_executable[page] || executable;
        }
        if (!section_layout_valid) break;
    }
    if (!section_layout_valid) {
        return KextRuntimeRollback(output, platform)
            ? KEXT_RUNTIME_STATUS::PagePermissionConflict
            : KEXT_RUNTIME_STATUS::RollbackFailed;
    }

    const uint64_t entry_page = image_info.entry_offset / KEXT_RUNTIME_PAGE_SIZE;
    if (entry_page >= used_pages || !page_executable[entry_page]) {
        return KextRuntimeRollback(output, platform)
            ? KEXT_RUNTIME_STATUS::EntryNotExecutable
            : KEXT_RUNTIME_STATUS::RollbackFailed;
    }

    // Replace the temporary RW+NX mapping with the final per-page policy.
    // Empty alignment/tail pages become RO+NX; code becomes RO+X; writable data
    // stays RW+NX. No page is ever mapped writable and executable together.
    for (uint32_t page = 0u; page < KEXT_RUNTIME_PAGE_COUNT; ++page) {
        const uint32_t final_flags = page_writable[page]
            ? starlix::mem::KERNEL_MODULE_MAP_WRITE
            : (page_executable[page]
                ? starlix::mem::KERNEL_MODULE_MAP_EXECUTE : 0u);
        if (final_flags == starlix::mem::KERNEL_MODULE_MAP_WRITE) {
            output->mapped_flags[page] = final_flags;
            continue;
        }
        const uint64_t va = module_virtual_base +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        const uint64_t pa = physical +
            static_cast<uint64_t>(page) * KEXT_RUNTIME_PAGE_SIZE;
        if (!platform->unmap_module_page(platform->context, va, pa, 1u)) {
            return KextRuntimeRollback(output, platform)
                ? KEXT_RUNTIME_STATUS::ModuleMappingFailed
                : KEXT_RUNTIME_STATUS::RollbackFailed;
        }
        output->mapped_flags[page] = KEXT_RUNTIME_PAGE_UNMAPPED;
        if (!platform->map_module_page(platform->context, va, pa, final_flags)) {
            return KextRuntimeRollback(output, platform)
                ? KEXT_RUNTIME_STATUS::ModuleMappingFailed
                : KEXT_RUNTIME_STATUS::RollbackFailed;
        }
        output->mapped_flags[page] = final_flags;
    }

    output->image_size = image_info.image_size;
    output->entry_address = image_info.entry_address;
    output->loaded_section_count = image_info.loaded_section_count;
    output->applied_relocation_count = image_info.applied_relocation_count;
    output->state = 2u;
    return KEXT_RUNTIME_STATUS::Ready;
}

inline bool KextRuntimeUnloadExternal(
    KEXT_RUNTIME_MODULE* module, const KEXT_RUNTIME_PLATFORM* platform) {
    if (!module || !platform || (module->state != 2u && module->state != 3u)) return false;
    return KextRuntimeRollback(module, platform);
}

// Production adapters: the kernel provides generic memory and page-table
// services supplied by the kernel platform adapter.
inline uint64_t KextRuntimeKernelAllocate(void*, uint64_t pages) {
    return starlix::mem::managed_alloc_phys_pages(pages);
}
inline bool KextRuntimeKernelRelease(void*, uint64_t base, uint64_t pages) {
    return starlix::mem::managed_release_phys_pages(base, pages);
}
inline bool KextRuntimeKernelIdentityPermissions(
    void*, uint64_t physical, uint32_t expected, uint32_t target) {
    return starlix::mem::address_space_set_kernel_identity_page_permissions(
        physical, expected, target);
}
inline bool KextRuntimeKernelMap(void*, uint64_t virtual_address,
                                 uint64_t physical, uint32_t flags) {
    return starlix::mem::address_space_map_kernel_module_page(
        virtual_address, physical, flags);
}
inline bool KextRuntimeKernelUnmap(void*, uint64_t virtual_address,
                                   uint64_t physical, uint32_t flags) {
    return starlix::mem::address_space_unmap_kernel_module_page(
        virtual_address, physical, flags);
}
inline bool KextRuntimeKernelGuard(void*, uint64_t virtual_address) {
    const uint64_t root = starlix::mem::address_space_kernel_root();
    return root != 0ULL &&
        starlix::mem::address_space_kernel_module_guard_unmapped(
            root, virtual_address);
}
inline uint8_t* KextRuntimeKernelResolve(void*, uint64_t virtual_address,
                                        uint64_t) {
    return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(virtual_address));
}
inline void KextRuntimeMakeKernelPlatform(KEXT_RUNTIME_PLATFORM& output) {
    output = KEXT_RUNTIME_PLATFORM{};
    output.allocate_pages = &KextRuntimeKernelAllocate;
    output.release_pages = &KextRuntimeKernelRelease;
    output.set_identity_permissions = &KextRuntimeKernelIdentityPermissions;
    output.map_module_page = &KextRuntimeKernelMap;
    output.unmap_module_page = &KextRuntimeKernelUnmap;
    output.guard_unmapped = &KextRuntimeKernelGuard;
    output.resolve_write_alias = &KextRuntimeKernelResolve;
}

} // namespace starlix::loader
