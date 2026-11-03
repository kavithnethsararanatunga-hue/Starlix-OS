// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::mem::mmio_policy {

constexpr uint64_t PAGE_SIZE = 4096ULL;
constexpr uint32_t EFI_MEMORY_MAPPED_IO = 11u;
constexpr uint64_t EFI_MEMORY_UC = 0x0000000000000001ULL;
constexpr uint64_t MAX_MEMORY_MAP_BYTES = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t MAX_MEMORY_MAP_DESCRIPTORS = 131072ULL;

constexpr uint64_t PTE_PWT = 1ULL << 3;
constexpr uint64_t PTE_PCD = 1ULL << 4;
constexpr uint64_t PTE_PAT_4K = 1ULL << 7;
constexpr uint64_t INVALID_CACHE_FLAGS = ~0ULL;

struct EfiMemoryDescriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct PhysicalRange {
    uint64_t base;
    uint64_t size;
};

inline bool add_overflow(uint64_t a, uint64_t b, uint64_t& result) {
    result = a + b;
    return result < a;
}

inline bool multiply_overflow(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0ULL && b > (~0ULL / a)) return true;
    result = a * b;
    return false;
}

inline bool valid_range(const PhysicalRange& range) {
    uint64_t end = 0ULL;
    return range.base != 0ULL &&
           range.size >= PAGE_SIZE &&
           (range.base & (PAGE_SIZE - 1ULL)) == 0ULL &&
           (range.size & (PAGE_SIZE - 1ULL)) == 0ULL &&
           !add_overflow(range.base, range.size, end) &&
           end > range.base;
}

/* The current address-space MMIO backend uses identity mappings below 4 GiB.
   Do not accept a BAR whose complete range exceeds that verified window. */
constexpr uint64_t KERNEL_MMIO_PHYSICAL_LIMIT = 0x100000000ULL;

inline bool valid_kernel_mmio_range(const PhysicalRange& range) {
    return valid_range(range) &&
           range.base < KERNEL_MMIO_PHYSICAL_LIMIT &&
           range.size <= KERNEL_MMIO_PHYSICAL_LIMIT - range.base;
}

/* The complete MMIO range must be contained in one UEFI
   EfiMemoryMappedIO descriptor explicitly marked EFI_MEMORY_UC. */
inline bool descriptor_covers_uc_mmio(
    const void* memory_map,
    uint64_t memory_map_size,
    uint64_t descriptor_size,
    const PhysicalRange& requested
) {
    if (!memory_map || !valid_kernel_mmio_range(requested) ||
        memory_map_size == 0ULL ||
        memory_map_size > MAX_MEMORY_MAP_BYTES ||
        descriptor_size < sizeof(EfiMemoryDescriptor) ||
        descriptor_size > 4096ULL ||
        (descriptor_size & 7ULL) != 0ULL ||
        (memory_map_size % descriptor_size) != 0ULL) {
        return false;
    }

    const uint64_t count = memory_map_size / descriptor_size;
    if (count == 0ULL || count > MAX_MEMORY_MAP_DESCRIPTORS)
        return false;

    uint64_t requested_end = 0ULL;
    if (add_overflow(requested.base, requested.size, requested_end))
        return false;

    const auto* bytes = static_cast<const uint8_t*>(memory_map);

    for (uint64_t i = 0ULL; i < count; ++i) {
        const EfiMemoryDescriptor* descriptor =
            reinterpret_cast<const EfiMemoryDescriptor*>(
                bytes + i * descriptor_size);

        if (descriptor->type != EFI_MEMORY_MAPPED_IO ||
            (descriptor->attribute & EFI_MEMORY_UC) == 0ULL ||
            descriptor->number_of_pages == 0ULL ||
            (descriptor->physical_start & (PAGE_SIZE - 1ULL)) != 0ULL) {
            continue;
        }

        uint64_t descriptor_bytes = 0ULL;
        if (multiply_overflow(descriptor->number_of_pages,
                              PAGE_SIZE, descriptor_bytes)) {
            continue;
        }

        uint64_t descriptor_end = 0ULL;
        if (add_overflow(descriptor->physical_start,
                         descriptor_bytes, descriptor_end)) {
            continue;
        }

        if (requested.base >= descriptor->physical_start &&
            requested_end <= descriptor_end) {
            return true;
        }
    }

    return false;
}

/* A malformed or overflowing GOP framebuffer range is rejected.
   A valid MMIO range must be completely disjoint from the framebuffer. */
inline bool disjoint_from_framebuffer(
    const PhysicalRange& mmio,
    const PhysicalRange& framebuffer
) {
    if (!valid_kernel_mmio_range(mmio)) return false;

    /* Only {0,0} represents an absent framebuffer. GOP framebuffer
       byte ranges need not have page-aligned lengths or base addresses. */
    if (framebuffer.base == 0ULL && framebuffer.size == 0ULL)
        return true;
    if (framebuffer.base == 0ULL || framebuffer.size == 0ULL)
        return false;

    uint64_t mmio_end = 0ULL;
    uint64_t framebuffer_end = 0ULL;

    if (add_overflow(mmio.base, mmio.size, mmio_end) ||
        add_overflow(framebuffer.base, framebuffer.size, framebuffer_end)) {
        return false;
    }

    return mmio_end <= framebuffer.base ||
           framebuffer_end <= mmio.base;
}

/* IA32_PAT consists of eight byte entries. A memory type value of zero
   is UC. Do not confuse UC- (7) with UC (0). */
inline bool select_uc_pat_index(uint64_t pat_msr, uint8_t& out_index) {
    out_index = 0xFFu;

    for (uint8_t index = 0u; index < 8u; ++index) {
        const uint8_t entry = static_cast<uint8_t>(
            (pat_msr >> (static_cast<uint32_t>(index) * 8u)) & 0xFFULL);

        if (entry == 0u) {
            out_index = index;
            return true;
        }
    }

    return false;
}

/* Convert a PAT index into the PWT, PCD and 4-KiB PAT PTE bits. */
inline uint64_t pat_index_to_pte_flags(uint8_t index) {
    if (index > 7u) return INVALID_CACHE_FLAGS;

    uint64_t flags = 0ULL;
    if ((index & 1u) != 0u) flags |= PTE_PWT;
    if ((index & 2u) != 0u) flags |= PTE_PCD;
    if ((index & 4u) != 0u) flags |= PTE_PAT_4K;
    return flags;
}


constexpr uint32_t CPUID_FEATURE_MSR = 1u << 5;
constexpr uint32_t CPUID_FEATURE_PAT = 1u << 16;

/* Runtime policy gate. The caller must not execute RDMSR for IA32_PAT
   unless both the MSR instruction and architectural PAT feature exist. */
inline bool select_uc_pat_flags(
    uint32_t max_basic_leaf,
    uint32_t leaf1_edx,
    uint64_t pat_msr,
    uint8_t& out_index,
    uint64_t& out_flags
) {
    out_index = 0xFFu;
    out_flags = INVALID_CACHE_FLAGS;

    const uint32_t required =
        CPUID_FEATURE_MSR | CPUID_FEATURE_PAT;

    if (max_basic_leaf < 1u ||
        (leaf1_edx & required) != required) {
        return false;
    }

    if (!select_uc_pat_index(pat_msr, out_index))
        return false;

    out_flags = pat_index_to_pte_flags(out_index);
    return out_flags != INVALID_CACHE_FLAGS;
}

} // namespace starlix::mem::mmio_policy
