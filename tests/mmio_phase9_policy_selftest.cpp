// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdio>
#include "../kernel/src/mem/mmio_policy.hpp"

using namespace starlix::mem::mmio_policy;

static unsigned int passed = 0;
static unsigned int failed = 0;

static void check(bool condition, const char* description) {
    if (condition) {
        std::printf("[PASS] %s\n", description);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", description);
        ++failed;
    }
}

int main() {
    EfiMemoryDescriptor descriptors[] = {
        {EFI_MEMORY_MAPPED_IO, 0u, 0xF0000000ULL, 0ULL,
         0x100ULL, EFI_MEMORY_UC},
        {7u, 0u, 0xE0000000ULL, 0ULL,
         0x100ULL, 0x8ULL},
        {EFI_MEMORY_MAPPED_IO, 0u, 0xD0000000ULL, 0ULL,
         0x100ULL, 0x8ULL}
    };

    const uint64_t map_size = sizeof(descriptors);
    const uint64_t stride = sizeof(EfiMemoryDescriptor);

    const PhysicalRange valid_mmio{0xF0001000ULL, 0x2000ULL};
    const PhysicalRange framebuffer{0xE0000000ULL, 0x100000ULL};

    check(valid_range(valid_mmio),
          "aligned nonzero MMIO range is accepted");

    check(valid_kernel_mmio_range(valid_mmio),
          "MMIO range inside the kernel DMA32 window is accepted");

    check(!valid_kernel_mmio_range(
              {0xFFFFF000ULL, 0x2000ULL}),
          "MMIO range crossing the 4-GiB identity-map limit is rejected");

    check(!valid_range({0ULL, 0x1000ULL}),
          "physical address zero is rejected");

    check(!valid_range({0xF0000001ULL, 0x1000ULL}),
          "unaligned physical base is rejected");

    check(!valid_range({0xF0000000ULL, 0x1001ULL}),
          "non-page-sized range is rejected");

    check(!valid_range({0xFFFFFFFFFFFFF000ULL, 0x2000ULL}),
          "physical range overflow is rejected");

    check(descriptor_covers_uc_mmio(
              descriptors, map_size, stride, valid_mmio),
          "UC EfiMemoryMappedIO descriptor covers requested BAR range");

    check(!descriptor_covers_uc_mmio(
              descriptors, map_size, stride,
              {0xE0000000ULL, 0x1000ULL}),
          "EfiConventionalMemory descriptor is not accepted as MMIO");

    check(!descriptor_covers_uc_mmio(
              descriptors, map_size, stride,
              {0xD0000000ULL, 0x1000ULL}),
          "MMIO descriptor without EFI_MEMORY_UC is rejected");

    check(!descriptor_covers_uc_mmio(
              descriptors, map_size, stride,
              {0xF00FF000ULL, 0x2000ULL}),
          "range extending beyond descriptor is rejected");

    check(!descriptor_covers_uc_mmio(
              descriptors, map_size, stride - 8ULL, valid_mmio),
          "undersized descriptor stride is rejected");

    check(!descriptor_covers_uc_mmio(
              descriptors, map_size - 1ULL, stride, valid_mmio),
          "partial descriptor at end of memory map is rejected");

    check(!descriptor_covers_uc_mmio(
              descriptors, MAX_MEMORY_MAP_BYTES + 1ULL,
              stride, valid_mmio),
          "oversized UEFI memory map is rejected");

    check(disjoint_from_framebuffer(valid_mmio, framebuffer),
          "non-overlapping MMIO and GOP framebuffer ranges are accepted");

    check(disjoint_from_framebuffer(
              valid_mmio, {0xE0000001ULL, 0x100003ULL}),
          "unaligned GOP framebuffer byte ranges are handled safely");

    check(!disjoint_from_framebuffer(
              valid_mmio, {0xE0000000ULL, 0ULL}),
          "partially absent framebuffer ranges are rejected");

    check(!disjoint_from_framebuffer(
              {0xE0001000ULL, 0x2000ULL}, framebuffer),
          "MMIO overlapping GOP framebuffer is rejected");

    check(!disjoint_from_framebuffer(
              valid_mmio, {0xFFFFFFFFFFFFF000ULL, 0x2000ULL}),
          "overflowing GOP framebuffer range is rejected");

    check(disjoint_from_framebuffer(valid_mmio, {0ULL, 0ULL}),
          "absent framebuffer range does not create a false overlap");

    uint8_t uc_index = 0xFFu;
    constexpr uint64_t standard_pat = 0x0007040600070406ULL;

    check(select_uc_pat_index(standard_pat, uc_index) && uc_index == 3u,
          "standard PAT selects the exact UC entry, not UC-minus");

    check(select_uc_pat_index(0x0600060606060606ULL, uc_index) &&
              uc_index == 6u,
          "UC entry can be discovered at a non-default PAT index");

    check(!select_uc_pat_index(0x0604060706040607ULL, uc_index) &&
              uc_index == 0xFFu,
          "PAT without an exact UC entry is rejected");

    uint64_t selected_uc_flags = INVALID_CACHE_FLAGS;
    uint8_t selected_uc_index = 0xFFu;
    constexpr uint32_t MSR_AND_PAT =
        CPUID_FEATURE_MSR | CPUID_FEATURE_PAT;

    check(select_uc_pat_flags(1u, MSR_AND_PAT, standard_pat,
              selected_uc_index, selected_uc_flags) &&
              selected_uc_index == 3u &&
              selected_uc_flags == (PTE_PWT | PTE_PCD),
          "runtime PAT policy selects exact UC after CPUID feature checks");

    check(!select_uc_pat_flags(0u, MSR_AND_PAT, standard_pat,
              selected_uc_index, selected_uc_flags) &&
              selected_uc_index == 0xFFu &&
              selected_uc_flags == INVALID_CACHE_FLAGS,
          "runtime PAT policy rejects a missing basic CPUID leaf");

    check(!select_uc_pat_flags(1u, CPUID_FEATURE_MSR, standard_pat,
              selected_uc_index, selected_uc_flags),
          "runtime PAT policy rejects CPUs without PAT support");

    check(!select_uc_pat_flags(1u, CPUID_FEATURE_PAT, standard_pat,
              selected_uc_index, selected_uc_flags),
          "runtime PAT policy rejects missing MSR instruction support");

    check(pat_index_to_pte_flags(3u) == (PTE_PWT | PTE_PCD),
          "PAT index 3 encodes PWT plus PCD");

    check(pat_index_to_pte_flags(4u) == PTE_PAT_4K,
          "PAT index 4 encodes the 4-KiB PAT bit");

    check(pat_index_to_pte_flags(7u) ==
              (PTE_PWT | PTE_PCD | PTE_PAT_4K),
          "PAT index 7 encodes all three cache-index bits");

    check(pat_index_to_pte_flags(8u) == INVALID_CACHE_FLAGS,
          "out-of-range PAT index is rejected");

    std::printf(
        "MMIO_PHASE9A_POLICY_SELFTEST: %u passed, %u failed\n",
        passed, failed);

    return failed == 0u ? 0 : 1;
}
