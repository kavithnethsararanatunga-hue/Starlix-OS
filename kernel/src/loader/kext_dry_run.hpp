// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#if !defined(STARLIX_PRIMITIVE_TYPES_DEFINED)
#include <stddef.h>
#include <stdint.h>
#endif
#include "../../../include/driver/sys_elf_loader.hpp"

namespace starlix::mem {
// Generic kernel memory services. External module implementations are not
// linked here; only temporary pages are leased for package validation/relocation.
uint64_t managed_alloc_phys_pages(uint64_t page_count);
bool managed_release_phys_pages(uint64_t physical_base, uint64_t page_count);
}

namespace starlix::sys {

constexpr uint64_t KEXT_DRY_RUN_PAGE_SIZE = 4096ULL;
constexpr uint64_t KEXT_DRY_RUN_IMAGE_CAPACITY = 64ULL * 1024ULL;
constexpr uint64_t KEXT_DRY_RUN_PAGE_COUNT =
    KEXT_DRY_RUN_IMAGE_CAPACITY / KEXT_DRY_RUN_PAGE_SIZE;
constexpr uint64_t KEXT_DRY_RUN_MAX_PACKAGE_SIZE = 1024ULL * 1024ULL;


struct KEXT_DRY_RUN_REPORT {
    SYS_VALIDATE_RESULT package_result;
    SYS_ELF_LOAD_RESULT relocation_result;
    uint64_t image_size;
    uint64_t image_alignment;
    uint64_t entry_offset;
    uint32_t loaded_section_count;
    uint32_t applied_relocation_count;
    uint32_t staging_page_count;
    uint32_t buffer_scrubbed;
    uint32_t pages_released;
    uint32_t reserved;
};

enum class KEXT_DRY_RUN_STATUS : uint32_t {
    Ready = 0,
    InvalidArgument,
    PackageRejected,
    MemoryUnavailable,
    MemoryInvalid,
    RelocationRejected,
    ReportRejected,
    ScrubFailed,
    ReleaseFailed
};

inline bool KextDryRunScrubAndRelease(uint64_t physical_base,
                                      uint8_t* image,
                                      uint64_t page_count,
                                      KEXT_DRY_RUN_REPORT* report) {
    if (!image || !report || physical_base == 0ULL || page_count == 0ULL)
        return false;

    volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(image);
    for (uint64_t i = 0; i < KEXT_DRY_RUN_IMAGE_CAPACITY; ++i)
        bytes[i] = 0u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    for (uint64_t i = 0; i < KEXT_DRY_RUN_IMAGE_CAPACITY; ++i) {
        if (bytes[i] != 0u) return false;
    }
    report->buffer_scrubbed = 1u;

    if (!starlix::mem::managed_release_phys_pages(physical_base, page_count))
        return false;
    report->pages_released = 1u;
    return true;
}

/*
 * Validate and relocate any external KEXT SYS package into a temporary,
 * reclaimable page lease. The image is scrubbed and released before return.
 * This is deliberately NOT an executable loader: no page permissions are
 * changed and the KEXT entry point is never called.
 */
inline KEXT_DRY_RUN_STATUS KextDryRunPrepareExternalKext(
    const uint8_t* package, uint64_t packageSize,
    KEXT_DRY_RUN_REPORT* report) {
    if (report) *report = KEXT_DRY_RUN_REPORT{};
    if (!package || !report || packageSize < sizeof(SYS_HEADER) ||
        packageSize > KEXT_DRY_RUN_MAX_PACKAGE_SIZE)
        return KEXT_DRY_RUN_STATUS::InvalidArgument;

    report->relocation_result = SYS_ELF_LOAD_RESULT::InvalidArgument;
    report->package_result = ValidateSysPackage(package, packageSize);
    if (report->package_result != SYS_VALIDATE_RESULT::Valid)
        return KEXT_DRY_RUN_STATUS::PackageRejected;

    const uint64_t physical_base =
        starlix::mem::managed_alloc_phys_pages(KEXT_DRY_RUN_PAGE_COUNT);
    if (physical_base == 0ULL)
        return KEXT_DRY_RUN_STATUS::MemoryUnavailable;
    report->staging_page_count =
        static_cast<uint32_t>(KEXT_DRY_RUN_PAGE_COUNT);

    if ((physical_base & (KEXT_DRY_RUN_PAGE_SIZE - 1ULL)) != 0ULL ||
        physical_base > (~0ULL - KEXT_DRY_RUN_IMAGE_CAPACITY)) {
        // Invalid allocator output cannot safely be dereferenced. Attempt a
        // lease return only when it still satisfies the allocator's alignment.
        if ((physical_base & (KEXT_DRY_RUN_PAGE_SIZE - 1ULL)) == 0ULL)
            (void)starlix::mem::managed_release_phys_pages(
                physical_base, KEXT_DRY_RUN_PAGE_COUNT);
        return KEXT_DRY_RUN_STATUS::MemoryInvalid;
    }

    auto* image = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(physical_base));
    SYS_ELF_LOAD_IMAGE image_info{};
    report->relocation_result = RelocateSysElf64Image(
        package, packageSize, image, KEXT_DRY_RUN_IMAGE_CAPACITY,
        physical_base, &image_info);

    KEXT_DRY_RUN_STATUS final_status = KEXT_DRY_RUN_STATUS::Ready;
    if (report->relocation_result != SYS_ELF_LOAD_RESULT::Valid) {
        final_status = KEXT_DRY_RUN_STATUS::RelocationRejected;
    } else if (image_info.image_size == 0ULL ||
        image_info.image_size > KEXT_DRY_RUN_IMAGE_CAPACITY ||
        image_info.entry_offset >= image_info.image_size ||
        image_info.entry_address != physical_base + image_info.entry_offset ||
        image_info.applied_relocation_count == 0u ||
        image_info.loaded_section_count == 0u) {
        final_status = KEXT_DRY_RUN_STATUS::ReportRejected;
    } else {
        report->image_size = image_info.image_size;
        report->image_alignment = image_info.image_alignment;
        report->entry_offset = image_info.entry_offset;
        report->loaded_section_count = image_info.loaded_section_count;
        report->applied_relocation_count = image_info.applied_relocation_count;
    }

    if (!KextDryRunScrubAndRelease(physical_base, image,
            KEXT_DRY_RUN_PAGE_COUNT, report)) {
        return report->buffer_scrubbed
            ? KEXT_DRY_RUN_STATUS::ReleaseFailed
            : KEXT_DRY_RUN_STATUS::ScrubFailed;
    }
    return final_status;
}

} // namespace starlix::sys
