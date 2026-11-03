// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "phys.hpp"
#include "phys_alloc_math.hpp"
#include "../bootinfo.hpp"

extern "C" uint8_t __starlix_kernel_start;
extern "C" uint8_t __starlix_kernel_end;

namespace starlix::mem {

static constexpr uint64_t PAGE_BYTES = detail::PHYS_PAGE_BYTES;
static constexpr uint64_t LOW_RESERVED_END = 0x00100000ULL;
static constexpr uint64_t DMA32_END_EXCLUSIVE = 0x100000000ULL;
static constexpr uint64_t MAX_MAP_BYTES = 16ULL * 1024ULL * 1024ULL;
static constexpr uint64_t MAX_MAP_DESCRIPTORS = 131072ULL;
static constexpr uint32_t EFI_CONVENTIONAL_MEMORY = 7u;

/* UEFI x64 EFI_MEMORY_DESCRIPTOR layout. Descriptor stride comes from boot info. */
struct EfiMemoryDescriptorLocal {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

static uint64_t phys_heap_current = 0;
static uint64_t phys_heap_limit = 0; /* Exclusive limit; always below or at 4 GiB. */
static uint32_t phys_allocator_state = 0; /* 0=not ready, 1=ready */

/* Fixed-capacity, boot-lifetime DMA mapping registry. The current x86 backend
   uses identity mappings, but CPU pointers and device addresses stay separate
   in the API so a future VMM/IOMMU backend does not need EHCI pointer casts. */
static constexpr uint32_t DMA32_REGION_CAPACITY = 512u;
static DmaRegion dma32_regions[DMA32_REGION_CAPACITY]{};
/* Reserve registry capacity before consuming physical RAM. Without this bitmap,
   allocations made after the registry fills would be permanently leaked. */
static bool dma32_region_reserved[DMA32_REGION_CAPACITY]{};
static uint32_t dma32_registry_lock = 0u;

static void dma32_lock_registry() {
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&dma32_registry_lock, &expected, 1u,
                false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static void dma32_unlock_registry() {
    __atomic_store_n(&dma32_registry_lock, 0u, __ATOMIC_RELEASE);
}

static bool dma32_is_power_of_two(uint64_t value) {
    return value != 0ULL && (value & (value - 1ULL)) == 0ULL;
}

static bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t& result) {
    result = a + b;
    return result < a;
}

static bool mul_overflow_u64(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > (~0ULL / a)) return true;
    result = a * b;
    return false;
}

static uint64_t align_up_page(uint64_t value) {
    if (value > (~0ULL - (PAGE_BYTES - 1ULL))) return 0;
    return (value + PAGE_BYTES - 1ULL) & ~(PAGE_BYTES - 1ULL);
}

bool phys_init(const StarlixBootInfo* boot_info) {
    if (__atomic_load_n(&phys_allocator_state, __ATOMIC_ACQUIRE) == 1u) return true;
    __atomic_store_n(&phys_allocator_state, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&phys_heap_current, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&phys_heap_limit, 0ULL, __ATOMIC_RELAXED);

    if (!boot_info || !boot_info->MemoryMapBase ||
        (boot_info->MemoryMapBase & 7ULL) != 0ULL || !boot_info->MemoryMapSize ||
        boot_info->MemoryMapSize > MAX_MAP_BYTES ||
        boot_info->MemoryMapBase > (~0ULL - boot_info->MemoryMapSize) ||
        boot_info->MemoryMapDescriptorSize < sizeof(EfiMemoryDescriptorLocal) ||
        boot_info->MemoryMapDescriptorSize > 4096ULL ||
        (boot_info->MemoryMapSize % boot_info->MemoryMapDescriptorSize) != 0ULL) {
        return false;
    }

    const uint64_t count = boot_info->MemoryMapSize / boot_info->MemoryMapDescriptorSize;
    if (count == 0 || count > MAX_MAP_DESCRIPTORS) return false;

    const uint64_t kernel_begin_raw = reinterpret_cast<uintptr_t>(&__starlix_kernel_start);
    const uint64_t kernel_end_raw = reinterpret_cast<uintptr_t>(&__starlix_kernel_end);
    if (kernel_end_raw <= kernel_begin_raw) return false;
    const uint64_t kernel_begin = kernel_begin_raw & ~(PAGE_BYTES - 1ULL);
    const uint64_t kernel_end = align_up_page(kernel_end_raw);
    if (kernel_end == 0 || kernel_end <= kernel_begin ||
        kernel_end > DMA32_END_EXCLUSIVE) return false;

    const uint8_t* map = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(boot_info->MemoryMapBase));
    uint64_t best_start = 0;
    uint64_t best_end = 0;
    uint64_t best_bytes = 0;

    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t* raw = map + i * boot_info->MemoryMapDescriptorSize;
        const EfiMemoryDescriptorLocal* desc =
            reinterpret_cast<const EfiMemoryDescriptorLocal*>(raw);
        if (desc->type != EFI_CONVENTIONAL_MEMORY || desc->number_of_pages == 0) continue;

        uint64_t region_bytes = 0;
        if (mul_overflow_u64(desc->number_of_pages, PAGE_BYTES, region_bytes)) continue;
        uint64_t region_end = 0;
        if (add_overflow_u64(desc->physical_start, region_bytes, region_end)) continue;

        uint64_t start = desc->physical_start < LOW_RESERVED_END
            ? LOW_RESERVED_END : desc->physical_start;
        start = align_up_page(start);
        uint64_t end = region_end < DMA32_END_EXCLUSIVE
            ? region_end : DMA32_END_EXCLUSIVE;
        end &= ~(PAGE_BYTES - 1ULL);
        if (start == 0 || end <= start) continue;

        /* Exclude any overlap with the linked kernel image. This is a fail-safe
           guard against a bad/stale firmware map ever causing alloc_phys_page()
           to zero the allocator's own globals, code, or other kernel data. */
        uint64_t candidate_starts[2] = { start, 0ULL };
        uint64_t candidate_ends[2] = { end, 0ULL };
        uint32_t candidate_count = 1u;
        if (start < kernel_end && kernel_begin < end) {
            candidate_count = 0u;
            if (start < kernel_begin) {
                candidate_starts[candidate_count] = start;
                candidate_ends[candidate_count] = kernel_begin < end ? kernel_begin : end;
                ++candidate_count;
            }
            if (kernel_end < end) {
                candidate_starts[candidate_count] = kernel_end > start ? kernel_end : start;
                candidate_ends[candidate_count] = end;
                ++candidate_count;
            }
        }
        for (uint32_t c = 0; c < candidate_count; ++c) {
            const uint64_t candidate_start = candidate_starts[c];
            const uint64_t candidate_end = candidate_ends[c];
            if (candidate_start == 0 || candidate_end <= candidate_start) continue;
            const uint64_t span = candidate_end - candidate_start;
            if (span > best_bytes) {
                best_start = candidate_start;
                best_end = candidate_end;
                best_bytes = span;
            }
        }
    }

    if (best_bytes < PAGE_BYTES) return false;

    /* Publish the selected free conventional-RAM interval only after validation. */
    __atomic_store_n(&phys_heap_limit, best_end, __ATOMIC_RELAXED);
    __atomic_store_n(&phys_heap_current, best_start, __ATOMIC_RELAXED);
    __atomic_store_n(&phys_allocator_state, 1u, __ATOMIC_RELEASE);
    return true;
}

bool phys_allocator_ready() {
    return __atomic_load_n(&phys_allocator_state, __ATOMIC_ACQUIRE) == 1u;
}

uint64_t alloc_phys_pages(uint64_t page_count) {
    if (!phys_allocator_ready()) return 0;

    uint64_t allocation_bytes = 0;
    if (!detail::phys_page_count_to_bytes(page_count, allocation_bytes))
        return 0;

    uint64_t page =
        __atomic_load_n(&phys_heap_current, __ATOMIC_RELAXED);

    for (;;) {
        const uint64_t limit =
            __atomic_load_n(&phys_heap_limit, __ATOMIC_RELAXED);

        uint64_t next = 0;
        if (!detail::phys_reservation_end(
                page, limit, allocation_bytes, next))
            return 0;

        uint64_t expected = page;
        if (__atomic_compare_exchange_n(
                &phys_heap_current, &expected, next, false,
                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            break;

        // Another allocator advanced the bump pointer; retry at its value.
        page = expected;
    }

    // Current x86-64 Starlix uses identity-mapped conventional RAM here.
    // The complete contiguous allocation is zeroed before it is returned.
    uint8_t* memory =
        reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(page));

    for (uint64_t offset = 0; offset < allocation_bytes; ++offset)
        memory[offset] = 0;

    return page;
}

uint64_t alloc_phys_page() {
    return alloc_phys_pages(1);
}

bool dma32_alloc_page(DmaRegion& out_region) {
    out_region = DmaRegion{};
    if (!phys_allocator_ready()) return false;

    /* Claim a registry slot first. The region remains invisible to resolve()
       until it is published as valid, while the reservation prevents another
       allocator from consuming the same slot. If the registry is full, do not
       consume a physical page that can never be registered or reclaimed. */
    uint32_t reserved_index = DMA32_REGION_CAPACITY;
    dma32_lock_registry();
    for (uint32_t i = 0; i < DMA32_REGION_CAPACITY; ++i) {
        if (!dma32_regions[i].valid && !dma32_region_reserved[i]) {
            dma32_region_reserved[i] = true;
            reserved_index = i;
            break;
        }
    }
    dma32_unlock_registry();
    if (reserved_index == DMA32_REGION_CAPACITY) return false;

    const uint64_t physical = alloc_phys_page();
    if (physical == 0ULL || physical >= DMA32_END_EXCLUSIVE ||
        PAGE_BYTES > DMA32_END_EXCLUSIVE - physical) {
        dma32_lock_registry();
        dma32_region_reserved[reserved_index] = false;
        dma32_unlock_registry();
        return false;
    }

    DmaRegion candidate{};
    /* Explicit current identity-mapped backend. Keeping these fields separate
       means callers use device_address only for hardware descriptors. */
    candidate.cpu_address = reinterpret_cast<void*>(static_cast<uintptr_t>(physical));
    candidate.device_address = physical;
    candidate.length = PAGE_BYTES;
    candidate.valid = true;

    dma32_lock_registry();
    /* reserved_index belongs exclusively to this caller until publication. */
    dma32_regions[reserved_index] = candidate;
    dma32_region_reserved[reserved_index] = false;
    dma32_unlock_registry();
    out_region = candidate;
    return true;
}

bool dma32_resolve(const void* cpu_address, uint64_t bytes,
                   uint64_t alignment, uint32_t& out_device_address) {
    out_device_address = 0u;
    if (!cpu_address || bytes == 0ULL || !dma32_is_power_of_two(alignment)) return false;

    const uint64_t cpu_start = reinterpret_cast<uintptr_t>(cpu_address);
    if (bytes - 1ULL > (~0ULL - cpu_start)) return false;

    DmaRegion matched{};
    uint64_t offset = 0ULL;
    bool found = false;
    dma32_lock_registry();
    for (uint32_t i = 0; i < DMA32_REGION_CAPACITY; ++i) {
        const DmaRegion& region = dma32_regions[i];
        if (!region.valid || !region.cpu_address || region.length == 0ULL) continue;
        const uint64_t region_start = reinterpret_cast<uintptr_t>(region.cpu_address);
        if (cpu_start < region_start) continue;
        const uint64_t candidate_offset = cpu_start - region_start;
        if (candidate_offset > region.length || bytes > region.length - candidate_offset) continue;
        matched = region;
        offset = candidate_offset;
        found = true;
        break;
    }
    dma32_unlock_registry();
    if (!found || !matched.valid || offset > (~0ULL - matched.device_address)) return false;

    const uint64_t device_start = matched.device_address + offset;
    if (device_start == 0ULL || device_start > 0xFFFFFFFFULL ||
        bytes - 1ULL > 0xFFFFFFFFULL - device_start ||
        (device_start & (alignment - 1ULL)) != 0ULL) return false;

    out_device_address = static_cast<uint32_t>(device_start);
    return true;
}

bool dma32_sync_for_device(const void* cpu_address, uint64_t bytes) {
    uint32_t device_address = 0u;
    if (!dma32_resolve(cpu_address, bytes, 1ULL, device_address)) return false;
#if defined(__x86_64__)
    /* x86-64 host DMA is coherent for the supported baseline. SFENCE makes
       prior CPU stores globally visible before an EHCI schedule/doorbell write.
       This is not an IOMMU mapping or a cache-maintenance implementation for
       non-coherent architectures. */
    __asm__ __volatile__("sfence" ::: "memory");
    return true;
#else
    (void)device_address;
    return false;
#endif
}

bool dma32_sync_for_cpu(const void* cpu_address, uint64_t bytes) {
    uint32_t device_address = 0u;
    if (!dma32_resolve(cpu_address, bytes, 1ULL, device_address)) return false;
#if defined(__x86_64__)
    /* The caller must first prove the relevant EHCI schedule has completed or
       stopped. MFENCE then orders CPU reads after the device completion state.
       It does not make active hardware ownership safe, and does not invalidate
       caches on a non-coherent architecture. */
    __asm__ __volatile__("mfence" ::: "memory");
    return true;
#else
    (void)device_address;
    return false;
#endif
}

} // namespace starlix::mem
