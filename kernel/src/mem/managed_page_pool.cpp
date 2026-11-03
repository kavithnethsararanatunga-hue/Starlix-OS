// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "managed_page_pool.hpp"

namespace starlix::mem {
namespace {

constexpr uint64_t PAGE_BYTES = 4096ULL;
constexpr uint32_t RECORD_CAPACITY = 32768u; // 128 MiB of individually tracked pages.
constexpr uint32_t HASH_CAPACITY = 65536u;
constexpr uint32_t HASH_MASK = HASH_CAPACITY - 1u;
constexpr uint32_t EXTENT_CAPACITY = 4096u;

struct PageRecord {
    uint64_t physical;
    uint64_t lease_base;
    uint32_t lease_count;
    uint32_t state; // 0=empty, 1=leased, 2=free.
};
struct FreeExtent { uint64_t base; uint64_t page_count; };

PageRecord g_records[HASH_CAPACITY]{};
FreeExtent g_free_extents[EXTENT_CAPACITY]{};
uint32_t g_record_count = 0u;
uint32_t g_free_extent_count = 0u;
uint32_t g_lock = 0u;
bool g_ready = false;

void lock_pool() {
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&g_lock, &expected, 1u, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        __asm__ __volatile__("pause" ::: "memory");
    }
}
void unlock_pool() {
    __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
}

uint32_t hash_page(uint64_t physical) {
    const uint64_t page_number = physical >> 12;
    return static_cast<uint32_t>(
        (page_number * 11400714819323198485ULL) & HASH_MASK);
}

uint32_t find_record(uint64_t physical, bool& found) {
    uint32_t index = hash_page(physical);
    for (uint32_t probe = 0u; probe < HASH_CAPACITY; ++probe) {
        const PageRecord& record = g_records[index];
        if (record.state == 0u) {
            found = false;
            return index;
        }
        if (record.physical == physical) {
            found = true;
            return index;
        }
        index = (index + 1u) & HASH_MASK;
    }
    found = false;
    return HASH_CAPACITY;
}

void zero_run(uint64_t physical, uint64_t page_count) {
    volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(
        static_cast<uintptr_t>(physical));
    const uint64_t total_bytes = page_count * PAGE_BYTES;
    for (uint64_t i = 0; i < total_bytes; ++i) bytes[i] = 0u;
}

/* The extent table is sorted, disjoint, and coalesced after each release. */
bool insert_free_extent(uint64_t base, uint64_t page_count) {
    uint64_t end = base + page_count * PAGE_BYTES;
    uint32_t position = 0u;
    while (position < g_free_extent_count &&
           g_free_extents[position].base < base) ++position;

    bool merge_left = false;
    bool merge_right = false;
    if (position > 0u) {
        const FreeExtent& left = g_free_extents[position - 1u];
        const uint64_t left_end = left.base + left.page_count * PAGE_BYTES;
        if (left_end > base) return false;
        merge_left = left_end == base;
    }
    if (position < g_free_extent_count) {
        const FreeExtent& right = g_free_extents[position];
        if (end > right.base) return false;
        merge_right = end == right.base;
    }

    if (merge_left && merge_right) {
        g_free_extents[position - 1u].page_count += page_count +
            g_free_extents[position].page_count;
        for (uint32_t i = position; i + 1u < g_free_extent_count; ++i)
            g_free_extents[i] = g_free_extents[i + 1u];
        --g_free_extent_count;
        return true;
    }
    if (merge_left) {
        g_free_extents[position - 1u].page_count += page_count;
        return true;
    }
    if (merge_right) {
        g_free_extents[position].base = base;
        g_free_extents[position].page_count += page_count;
        return true;
    }
    if (g_free_extent_count == EXTENT_CAPACITY) return false;
    for (uint32_t i = g_free_extent_count; i > position; --i)
        g_free_extents[i] = g_free_extents[i - 1u];
    g_free_extents[position] = FreeExtent{base, page_count};
    ++g_free_extent_count;
    return true;
}

} // namespace

bool managed_page_pool_init() {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return true;
    if (!phys_allocator_ready()) return false;
    lock_pool();
    const bool pristine = g_record_count == 0u && g_free_extent_count == 0u;
    if (pristine) __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    unlock_pool();
    return pristine;
}

bool managed_page_pool_ready() {
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE);
}

uint64_t managed_alloc_phys_pages(uint64_t page_count) {
    if (page_count == 0ULL || page_count > RECORD_CAPACITY ||
        !phys_allocator_ready() || !managed_page_pool_ready()) return 0ULL;

    lock_pool();

    /* Reuse a coalesced extent first, including for multi-page contiguous runs. */
    for (uint32_t extent_index = 0u; extent_index < g_free_extent_count;
         ++extent_index) {
        FreeExtent& extent = g_free_extents[extent_index];
        if (extent.page_count < page_count) continue;
        const uint64_t physical = extent.base;
        bool records_valid = true;
        for (uint64_t page = 0; page < page_count; ++page) {
            bool found = false;
            const uint32_t index = find_record(physical + page * PAGE_BYTES, found);
            if (index == HASH_CAPACITY || !found || g_records[index].state != 2u) {
                records_valid = false;
                break;
            }
        }
        if (!records_valid) {
            unlock_pool();
            return 0ULL;
        }
        for (uint64_t page = 0; page < page_count; ++page) {
            bool found = false;
            const uint32_t index = find_record(physical + page * PAGE_BYTES, found);
            g_records[index].state = 1u;
            g_records[index].lease_base = physical;
            g_records[index].lease_count = static_cast<uint32_t>(page_count);
        }
        extent.base += page_count * PAGE_BYTES;
        extent.page_count -= page_count;
        if (extent.page_count == 0ULL) {
            for (uint32_t i = extent_index; i + 1u < g_free_extent_count; ++i)
                g_free_extents[i] = g_free_extents[i + 1u];
            --g_free_extent_count;
        }
        unlock_pool();
        zero_run(physical, page_count);
        return physical;
    }

    if (page_count > static_cast<uint64_t>(RECORD_CAPACITY - g_record_count)) {
        unlock_pool();
        return 0ULL;
    }
    const uint64_t physical = alloc_phys_pages(page_count);
    if (physical == 0ULL ||
        page_count > ((~0ULL - physical) / PAGE_BYTES)) {
        unlock_pool();
        return 0ULL;
    }

    for (uint64_t page = 0; page < page_count; ++page) {
        const uint64_t address = physical + page * PAGE_BYTES;
        bool found = false;
        const uint32_t index = find_record(address, found);
        if (index == HASH_CAPACITY || found) {
            /* Monotonic backing allocator makes this unreachable unless its
               ownership contract has been violated. Fail closed on corruption. */
            unlock_pool();
            return 0ULL;
        }
        g_records[index].physical = address;
        g_records[index].lease_base = physical;
        g_records[index].lease_count = static_cast<uint32_t>(page_count);
        g_records[index].state = 1u;
        ++g_record_count;
    }

    unlock_pool();
    return physical;
}

bool managed_release_phys_pages(uint64_t physical_base, uint64_t page_count) {
    if (!managed_page_pool_ready() || physical_base == 0ULL ||
        (physical_base & (PAGE_BYTES - 1ULL)) != 0ULL ||
        page_count == 0ULL || page_count > RECORD_CAPACITY ||
        page_count > ((~0ULL - physical_base) / PAGE_BYTES)) return false;

    lock_pool();
    /* Validate the entire lease before changing records or free extents. */
    for (uint64_t page = 0; page < page_count; ++page) {
        const uint64_t address = physical_base + page * PAGE_BYTES;
        bool found = false;
        const uint32_t index = find_record(address, found);
        if (index == HASH_CAPACITY || !found || g_records[index].state != 1u ||
            g_records[index].lease_base != physical_base ||
            g_records[index].lease_count != page_count) {
            unlock_pool();
            return false;
        }
    }

    if (!insert_free_extent(physical_base, page_count)) {
        unlock_pool();
        return false;
    }
    for (uint64_t page = 0; page < page_count; ++page) {
        const uint64_t address = physical_base + page * PAGE_BYTES;
        bool found = false;
        const uint32_t index = find_record(address, found);
        g_records[index].state = 2u;
        g_records[index].lease_base = 0ULL;
        g_records[index].lease_count = 0u;
    }
    unlock_pool();
    return true;
}

uint64_t managed_alloc_phys_page() { return managed_alloc_phys_pages(1ULL); }
bool managed_release_phys_page(uint64_t physical_page) {
    return managed_release_phys_pages(physical_page, 1ULL);
}

} // namespace starlix::mem
