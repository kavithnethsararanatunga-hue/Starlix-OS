// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "address_space.hpp"

namespace starlix::mem {
namespace {

constexpr uint64_t PAGE_PRESENT = 1ULL << 0;
constexpr uint64_t PAGE_WRITABLE = 1ULL << 1;
constexpr uint64_t PAGE_USER = 1ULL << 2;
constexpr uint64_t PAGE_HUGE = 1ULL << 7;
constexpr uint64_t PAGE_ACCESSED = 1ULL << 5;
constexpr uint64_t KERNEL_STACK_PTE_FLAGS = PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX;
constexpr uint64_t KERNEL_MODULE_TABLE_FLAGS = PAGE_PRESENT | PAGE_WRITABLE;
constexpr uint64_t KERNEL_MMIO_TABLE_FLAGS =
    PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX;
constexpr uint64_t PAGE_PWT = 1ULL << 3;
constexpr uint64_t PAGE_PCD = 1ULL << 4;
constexpr uint64_t PAGE_NX = user_address::PTE_NX;
constexpr uint64_t CR0_WP = 1ULL << 16;
constexpr uint64_t CR0_PG = 1ULL << 31;
constexpr uint64_t CR0_PE = 1ULL << 0;
constexpr uint64_t CR4_PAE = 1ULL << 5;
constexpr uint64_t CR4_LA57 = 1ULL << 12;
constexpr uint64_t CR4_PCIDE = 1ULL << 17;
constexpr uint64_t EFER_LMA = 1ULL << 10;
constexpr uint64_t EFER_NXE = 1ULL << 11;
constexpr uint64_t PML4_INDEX_MASK = 0x1FFULL;

struct AddressSpaceRecord {
    uint64_t root_physical;
    bool valid;
};

struct KernelMmioRecord {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t cache_flags;
    uint64_t identity_old_cache_flags;
    bool active;
};

AddressSpacePlatform g_platform{};
uint64_t g_kernel_root = 0;
uint64_t g_active_root = 0;
uint64_t g_physical_mask = 0;
AddressSpaceRecord g_spaces[ADDRESS_SPACE_MAX]{};
uint32_t g_space_count = 0;
uint32_t g_lock = 0;
uint64_t g_tlb_invalidation_generation = 0ULL;
bool g_ready = false;
const char* g_failure_reason = "NOT_RUN";
const char* g_stack_mapping_failure_reason = "NOT_RUN";
const char* g_module_mapping_failure_reason = "NOT_RUN";
const char* g_identity_permission_failure_reason = "NOT_RUN";
const char* g_mmio_mapping_failure_reason = "NOT_RUN";
KernelMmioRecord g_mmio_records[KERNEL_MMIO_MAX_PAGES]{};

void lock_spaces() {
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_lock, &expected, 1u, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        asm volatile("pause" ::: "memory");
    }
}

void unlock_spaces() {
    __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
}

/* All shared kernel mapping changes pass through this function. The current
   platform is explicitly single-CPU, so the callback is local INVLPG. When
   AP startup is implemented, manager initialization must remain fail-closed
   until this adapter is backed by a synchronous all-online-CPU shootdown. */
void invalidate_shared_kernel_page(uint64_t virtual_address) {
    g_platform.invalidate_page(virtual_address);
    __atomic_add_fetch(&g_tlb_invalidation_generation, 1ULL, __ATOMIC_RELEASE);
}

bool physical_page_aligned(uint64_t value) {
    return (value & (user_address::PAGE_SIZE - 1ULL)) == 0ULL;
}

bool valid_physical_page(uint64_t physical) {
    return physical_page_aligned(physical) && physical != 0ULL &&
           (physical & ~g_physical_mask) == 0ULL;
}

volatile uint64_t* map_table(uint64_t physical) {
    if (!valid_physical_page(physical)) return nullptr;
    return g_platform.map_table(physical);
}

bool is_registered_space(const AddressSpace& space) {
    if (space.magic != ADDRESS_SPACE_MAGIC ||
        !valid_physical_page(space.cr3)) return false;
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i) {
        if (g_spaces[i].valid &&
            g_spaces[i].root_physical == space.cr3) return true;
    }
    return false;
}

bool allocate_zeroed_table(uint64_t& out_physical,
                            volatile uint64_t*& out_table) {
    out_physical = 0ULL;
    out_table = nullptr;
    const uint64_t physical = g_platform.allocate_page();
    if (!valid_physical_page(physical)) return false;
    volatile uint64_t* table = map_table(physical);
    if (!table) {
        if (g_platform.release_page) (void)g_platform.release_page(physical);
        return false;
    }
    for (uint32_t i = 0; i < 512u; ++i) table[i] = 0ULL;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    out_physical = physical;
    out_table = table;
    return true;
}

bool ensure_child_table(volatile uint64_t* parent, uint32_t index,
                        volatile uint64_t*& out_child) {
    out_child = nullptr;
    if (!parent || index >= 512u) return false;
    const uint64_t entry = parent[index];

    if ((entry & PAGE_PRESENT) != 0ULL) {
        if ((entry & (PAGE_USER | PAGE_WRITABLE)) !=
                (PAGE_USER | PAGE_WRITABLE) ||
            (entry & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) return false;
        const uint64_t child_physical = entry & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(child_physical)) return false;
        out_child = map_table(child_physical);
        return out_child != nullptr;
    }

    /* Empty means exactly zero. Do not overwrite non-present software metadata. */
    if (entry != 0ULL) return false;

    uint64_t child_physical = 0ULL;
    volatile uint64_t* child = nullptr;
    if (!allocate_zeroed_table(child_physical, child)) return false;
    parent[index] = child_physical | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    out_child = child;
    return true;
}

bool walk_to_leaf(const AddressSpace& space, uint64_t virtual_address,
                  volatile uint64_t*& out_leaf_table,
                  uint32_t& out_leaf_index) {
    out_leaf_table = nullptr;
    out_leaf_index = 0;
    if (!is_registered_space(space) ||
        !user_address::is_user_address(virtual_address)) return false;

    volatile uint64_t* pml4 = map_table(space.cr3);
    if (!pml4) return false;

    const uint32_t pml4_index =
        static_cast<uint32_t>((virtual_address >> 39) & PML4_INDEX_MASK);
    const uint32_t pdpt_index =
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK);
    const uint32_t pd_index =
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK);
    const uint32_t pt_index =
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK);

    if (pml4_index != 1u) return false;
    volatile uint64_t* pdpt = nullptr;
    volatile uint64_t* pd = nullptr;
    volatile uint64_t* pt = nullptr;
    if (!ensure_child_table(pml4, pml4_index, pdpt) ||
        !ensure_child_table(pdpt, pdpt_index, pd) ||
        !ensure_child_table(pd, pd_index, pt)) return false;

    out_leaf_table = pt;
    out_leaf_index = pt_index;
    return true;
}

bool walk_existing_leaf(const AddressSpace& space, uint64_t virtual_address,
                        volatile uint64_t*& out_leaf_table,
                        uint32_t& out_leaf_index) {
    out_leaf_table = nullptr;
    out_leaf_index = 0;
    if (!is_registered_space(space) ||
        !user_address::is_user_address(virtual_address)) return false;

    volatile uint64_t* pml4 = map_table(space.cr3);
    if (!pml4) return false;
    const uint32_t pml4_index =
        static_cast<uint32_t>((virtual_address >> 39) & PML4_INDEX_MASK);
    const uint32_t pdpt_index =
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK);
    const uint32_t pd_index =
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK);
    const uint32_t pt_index =
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK);
    if (pml4_index != 1u) return false;

    const uint64_t pml4e = pml4[pml4_index];
    if ((pml4e & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pml4e & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) return false;
    volatile uint64_t* pdpt = map_table(pml4e & user_address::PTE_ADDRESS_MASK);
    if (!pdpt) return false;

    const uint64_t pdpte = pdpt[pdpt_index];
    if ((pdpte & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pdpte & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) return false;
    volatile uint64_t* pd = map_table(pdpte & user_address::PTE_ADDRESS_MASK);
    if (!pd) return false;

    const uint64_t pde = pd[pd_index];
    if ((pde & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pde & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) return false;
    volatile uint64_t* pt = map_table(pde & user_address::PTE_ADDRESS_MASK);
    if (!pt) return false;

    out_leaf_table = pt;
    out_leaf_index = pt_index;
    return true;
}

bool table_is_empty(const volatile uint64_t* table) {
    if (!table) return false;
    for (uint32_t i = 0; i < 512u; ++i)
        if (table[i] != 0ULL) return false;
    return true;
}

/* Reclaim only empty user page-table nodes. The target AddressSpace must be
   inactive (the public map/unmap APIs enforce that invariant). */
void prune_empty_user_path(const AddressSpace& space,
                           uint64_t virtual_address) {
    if (!g_platform.release_page || !is_registered_space(space) ||
        !user_address::is_user_address(virtual_address)) return;

    volatile uint64_t* pml4 = map_table(space.cr3);
    if (!pml4) return;
    const uint32_t pml4_index =
        static_cast<uint32_t>((virtual_address >> 39) & PML4_INDEX_MASK);
    const uint32_t pdpt_index =
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK);
    const uint32_t pd_index =
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK);

    const uint64_t pml4e = pml4[pml4_index];
    if ((pml4e & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pml4e & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) return;
    const uint64_t pdpt_physical = pml4e & user_address::PTE_ADDRESS_MASK;
    volatile uint64_t* pdpt = map_table(pdpt_physical);
    if (!pdpt) return;

    const uint64_t pdpte = pdpt[pdpt_index];
    if ((pdpte & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pdpte & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) {
        if (table_is_empty(pdpt)) {
            pml4[pml4_index] = 0ULL;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            (void)g_platform.release_page(pdpt_physical);
        }
        return;
    }
    const uint64_t pd_physical = pdpte & user_address::PTE_ADDRESS_MASK;
    volatile uint64_t* pd = map_table(pd_physical);
    if (!pd) return;

    const uint64_t pde = pd[pd_index];
    if ((pde & (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) !=
            (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE) ||
        (pde & (PAGE_HUGE | user_address::PTE_NX)) != 0ULL) {
        if (table_is_empty(pd)) {
            pdpt[pdpt_index] = 0ULL;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            (void)g_platform.release_page(pd_physical);
        }
        if (table_is_empty(pdpt)) {
            pml4[pml4_index] = 0ULL;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            (void)g_platform.release_page(pdpt_physical);
        }
        return;
    }
    const uint64_t pt_physical = pde & user_address::PTE_ADDRESS_MASK;
    volatile uint64_t* pt = map_table(pt_physical);
    if (!pt || !table_is_empty(pt)) return;

    pd[pd_index] = 0ULL;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    (void)g_platform.release_page(pt_physical);

    if (table_is_empty(pd)) {
        pdpt[pdpt_index] = 0ULL;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        (void)g_platform.release_page(pd_physical);
    }
    if (table_is_empty(pdpt)) {
        pml4[pml4_index] = 0ULL;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        (void)g_platform.release_page(pdpt_physical);
    }
}

bool registered_root(uint64_t root_physical) {
    if (root_physical == g_kernel_root && root_physical != 0ULL) return true;
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i) {
        if (g_spaces[i].valid &&
            g_spaces[i].root_physical == root_physical) return true;
    }
    return false;
}

bool valid_kernel_stack_page_address(uint64_t address) {
    return user_address::is_canonical_48(address) &&
           user_address::page_aligned(address) &&
           address >= KERNEL_STACK_REGION_BASE &&
           address < KERNEL_STACK_REGION_END &&
           ((address >> 39) & PML4_INDEX_MASK) ==
               KERNEL_STACK_PML4_INDEX;
}

bool kernel_stack_root_table(uint64_t root_physical,
                             volatile uint64_t*& out_root) {
    out_root = nullptr;
    if (!registered_root(root_physical)) {
        g_stack_mapping_failure_reason = "ROOT_NOT_REGISTERED";
        return false;
    }
    volatile uint64_t* root = map_table(root_physical);
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!root || !kernel_root) {
        g_stack_mapping_failure_reason = "ROOT_NOT_IDENTITY_ACCESSIBLE";
        return false;
    }
    const uint64_t kernel_entry = kernel_root[KERNEL_STACK_PML4_INDEX];
    const uint64_t root_entry = root[KERNEL_STACK_PML4_INDEX];
    if ((kernel_entry & (PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX)) !=
            (PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX) ||
        (kernel_entry & PAGE_USER) != 0ULL ||
        (root_entry & ~PAGE_ACCESSED) != (kernel_entry & ~PAGE_ACCESSED)) {
        g_stack_mapping_failure_reason = "SHARED_PML4_ENTRY_MISMATCH";
        return false;
    }
    out_root = root;
    return true;
}

bool walk_kernel_stack_leaf(volatile uint64_t* root,
                            uint64_t virtual_address,
                            bool create,
                            volatile uint64_t*& out_leaf,
                            uint32_t& out_index) {
    out_leaf = nullptr;
    out_index = 0u;
    if (!root || !valid_kernel_stack_page_address(virtual_address)) return false;

    volatile uint64_t* pdpt = nullptr;
    volatile uint64_t* pd = nullptr;
    volatile uint64_t* pt = nullptr;
    uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };
    volatile uint64_t* tables[3] = {nullptr, nullptr, nullptr};
    volatile uint64_t* parent = map_table(
        root[KERNEL_STACK_PML4_INDEX] & user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;

    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];
        if ((entry & PAGE_PRESENT) != 0ULL) {
            if ((entry & (PAGE_USER | PAGE_WRITABLE | user_address::PTE_NX)) !=
                    (PAGE_WRITABLE | user_address::PTE_NX) ||
                (entry & PAGE_HUGE) != 0ULL) return false;
            const uint64_t child_physical =
                entry & user_address::PTE_ADDRESS_MASK;
            if (!valid_physical_page(child_physical)) return false;
            tables[level] = map_table(child_physical);
            if (!tables[level]) return false;
        } else {
            if (!create || entry != 0ULL) return false;
            uint64_t child_physical = 0ULL;
            volatile uint64_t* child = nullptr;
            if (!allocate_zeroed_table(child_physical, child)) return false;
            parent[indices[level]] = child_physical | KERNEL_STACK_PTE_FLAGS;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            tables[level] = child;
        }
        parent = tables[level];
    }

    pdpt = tables[0];
    pd = tables[1];
    (void)pdpt;
    (void)pd;
    pt = parent;
    out_leaf = pt;
    out_index = indices[2];
    return true;
}

bool walk_kernel_stack_leaf_readonly(volatile uint64_t* root,
                                     uint64_t virtual_address,
                                     uint64_t& out_entry) {
    out_entry = 0ULL;
    if (!root || !valid_kernel_stack_page_address(virtual_address)) return false;
    volatile uint64_t* parent = map_table(
        root[KERNEL_STACK_PML4_INDEX] & user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;
    const uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };
    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];
        if ((entry & PAGE_PRESENT) == 0ULL) return true;
        if ((entry & (PAGE_USER | PAGE_WRITABLE | user_address::PTE_NX)) !=
                (PAGE_WRITABLE | user_address::PTE_NX) ||
            (entry & PAGE_HUGE) != 0ULL) return false;
        const uint64_t child_physical = entry & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(child_physical)) return false;
        parent = map_table(child_physical);
        if (!parent) return false;
    }
    out_entry = parent[indices[2]];
    return true;
}


bool valid_kernel_module_region_page_address(uint64_t address) {
    return user_address::is_canonical_48(address) &&
           user_address::page_aligned(address) &&
           address >= KERNEL_MODULE_REGION_BASE &&
           address < KERNEL_MODULE_REGION_END &&
           ((address >> 39) & PML4_INDEX_MASK) == KERNEL_MODULE_PML4_INDEX;
}

bool valid_kernel_module_page_address(uint64_t address) {
    return valid_kernel_module_region_page_address(address) &&
           address >= KERNEL_MODULE_REGION_BASE + user_address::PAGE_SIZE &&
           address < KERNEL_MODULE_REGION_END - user_address::PAGE_SIZE;
}

bool kernel_module_root_table(uint64_t root_physical,
                              volatile uint64_t*& out_root) {
    out_root = nullptr;
    if (!registered_root(root_physical)) {
        g_module_mapping_failure_reason = "ROOT_NOT_REGISTERED";
        return false;
    }
    volatile uint64_t* root = map_table(root_physical);
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!root || !kernel_root) {
        g_module_mapping_failure_reason = "ROOT_NOT_IDENTITY_ACCESSIBLE";
        return false;
    }
    const uint64_t kernel_entry = kernel_root[KERNEL_MODULE_PML4_INDEX];
    const uint64_t root_entry = root[KERNEL_MODULE_PML4_INDEX];
    if ((kernel_entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                         PAGE_NX | PAGE_HUGE)) !=
            (PAGE_PRESENT | PAGE_WRITABLE) ||
        (root_entry & ~PAGE_ACCESSED) != (kernel_entry & ~PAGE_ACCESSED)) {
        g_module_mapping_failure_reason = "SHARED_MODULE_PML4_ENTRY_MISMATCH";
        return false;
    }
    out_root = root;
    return true;
}

bool walk_kernel_module_leaf(volatile uint64_t* root,
                             uint64_t virtual_address,
                             bool create,
                             volatile uint64_t*& out_leaf,
                             uint32_t& out_index) {
    out_leaf = nullptr;
    out_index = 0u;
    if (!root || !valid_kernel_module_page_address(virtual_address)) return false;

    const uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };
    volatile uint64_t* parent = map_table(
        root[KERNEL_MODULE_PML4_INDEX] & user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;

    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];
        if ((entry & PAGE_PRESENT) != 0ULL) {
            if ((entry & (PAGE_USER | PAGE_NX | PAGE_HUGE)) != 0ULL ||
                (entry & PAGE_WRITABLE) == 0ULL) return false;
            const uint64_t child_physical =
                entry & user_address::PTE_ADDRESS_MASK;
            if (!valid_physical_page(child_physical)) return false;
            parent = map_table(child_physical);
            if (!parent) return false;
        } else {
            if (!create || entry != 0ULL) return false;
            uint64_t child_physical = 0ULL;
            volatile uint64_t* child = nullptr;
            if (!allocate_zeroed_table(child_physical, child) || !child)
                return false;
            parent[indices[level]] = child_physical | KERNEL_MODULE_TABLE_FLAGS;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            parent = child;
        }
    }
    out_leaf = parent;
    out_index = indices[2];
    return true;
}

bool walk_kernel_module_leaf_readonly(volatile uint64_t* root,
                                      uint64_t virtual_address,
                                      uint64_t& out_entry) {
    out_entry = 0ULL;
    if (!root || !valid_kernel_module_region_page_address(virtual_address)) return false;
    volatile uint64_t* parent = map_table(
        root[KERNEL_MODULE_PML4_INDEX] & user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;
    const uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };
    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];
        if ((entry & PAGE_PRESENT) == 0ULL) return entry == 0ULL;
        if ((entry & (PAGE_USER | PAGE_NX | PAGE_HUGE)) != 0ULL ||
            (entry & PAGE_WRITABLE) == 0ULL) return false;
        const uint64_t child_physical = entry & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(child_physical)) return false;
        parent = map_table(child_physical);
        if (!parent) return false;
    }
    out_entry = parent[indices[2]];
    return true;
}


bool valid_kernel_mmio_region_page_address(uint64_t address) {
    return user_address::is_canonical_48(address) &&
           user_address::page_aligned(address) &&
           address >= KERNEL_MMIO_REGION_BASE &&
           address < KERNEL_MMIO_REGION_END &&
           ((address >> 39) & PML4_INDEX_MASK) ==
               KERNEL_MMIO_PML4_INDEX;
}

bool valid_kernel_mmio_page_address(uint64_t address) {
    return valid_kernel_mmio_region_page_address(address) &&
           address >= KERNEL_MMIO_REGION_BASE + user_address::PAGE_SIZE &&
           address < KERNEL_MMIO_REGION_END - user_address::PAGE_SIZE;
}

bool kernel_mmio_root_table(uint64_t root_physical,
                            volatile uint64_t*& out_root) {
    out_root = nullptr;
    if (!registered_root(root_physical)) {
        g_mmio_mapping_failure_reason = "ROOT_NOT_REGISTERED";
        return false;
    }

    volatile uint64_t* root = map_table(root_physical);
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!root || !kernel_root) {
        g_mmio_mapping_failure_reason = "ROOT_NOT_IDENTITY_ACCESSIBLE";
        return false;
    }

    const uint64_t required =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX;
    const uint64_t mask =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
        PAGE_NX | PAGE_HUGE;

    const uint64_t kernel_entry = kernel_root[KERNEL_MMIO_PML4_INDEX];
    const uint64_t root_entry = root[KERNEL_MMIO_PML4_INDEX];

    if ((kernel_entry & mask) != required ||
        (root_entry & ~PAGE_ACCESSED) !=
            (kernel_entry & ~PAGE_ACCESSED)) {
        g_mmio_mapping_failure_reason =
            "SHARED_MMIO_PML4_ENTRY_MISMATCH";
        return false;
    }

    out_root = root;
    return true;
}

bool walk_kernel_mmio_leaf(volatile uint64_t* root,
                           uint64_t virtual_address,
                           bool create,
                           volatile uint64_t*& out_leaf,
                           uint32_t& out_index) {
    out_leaf = nullptr;
    out_index = 0u;

    if (!root || !valid_kernel_mmio_page_address(virtual_address))
        return false;

    const uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };

    volatile uint64_t* parent = map_table(
        root[KERNEL_MMIO_PML4_INDEX] &
        user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;

    const uint64_t required =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX;
    const uint64_t mask =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
        PAGE_NX | PAGE_HUGE;

    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];

        if ((entry & PAGE_PRESENT) != 0ULL) {
            if ((entry & mask) != required) return false;

            const uint64_t child_physical =
                entry & user_address::PTE_ADDRESS_MASK;
            if (!valid_physical_page(child_physical)) return false;

            parent = map_table(child_physical);
            if (!parent) return false;
        } else {
            if (!create || entry != 0ULL) return false;

            uint64_t child_physical = 0ULL;
            volatile uint64_t* child = nullptr;
            if (!allocate_zeroed_table(child_physical, child) || !child)
                return false;

            parent[indices[level]] =
                child_physical | KERNEL_MMIO_TABLE_FLAGS;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            parent = child;
        }
    }

    out_leaf = parent;
    out_index = indices[2];
    return true;
}

bool walk_kernel_mmio_leaf_readonly(volatile uint64_t* root,
                                    uint64_t virtual_address,
                                    uint64_t& out_entry) {
    out_entry = 0ULL;
    if (!root ||
        !valid_kernel_mmio_region_page_address(virtual_address))
        return false;

    volatile uint64_t* parent = map_table(
        root[KERNEL_MMIO_PML4_INDEX] &
        user_address::PTE_ADDRESS_MASK);
    if (!parent) return false;

    const uint32_t indices[3] = {
        static_cast<uint32_t>((virtual_address >> 30) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 21) & PML4_INDEX_MASK),
        static_cast<uint32_t>((virtual_address >> 12) & PML4_INDEX_MASK)
    };

    const uint64_t required =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX;
    const uint64_t mask =
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
        PAGE_NX | PAGE_HUGE;

    for (uint32_t level = 0; level < 2u; ++level) {
        const uint64_t entry = parent[indices[level]];

        if ((entry & PAGE_PRESENT) == 0ULL)
            return entry == 0ULL;

        if ((entry & mask) != required) return false;

        const uint64_t child_physical =
            entry & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(child_physical)) return false;

        parent = map_table(child_physical);
        if (!parent) return false;
    }

    out_entry = parent[indices[2]];
    return true;
}

bool query_kernel_mapping_unlocked(uint64_t root_physical,
                                   uint64_t virtual_address,
                                   KernelMappingInfo& out_info) {
    out_info = KernelMappingInfo{};
    if (!registered_root(root_physical) ||
        !user_address::is_canonical_48(virtual_address)) return false;
    volatile uint64_t* table = map_table(root_physical);
    if (!table) return false;

    bool writable = true;
    bool executable = true;
    bool user = false;
    for (uint32_t level = 4u; level != 0u; --level) {
        const uint32_t shift = 12u + 9u * (level - 1u);
        const uint32_t index = static_cast<uint32_t>(
            (virtual_address >> shift) & PML4_INDEX_MASK);
        const uint64_t entry = table[index];
        if ((entry & PAGE_PRESENT) == 0ULL ||
            (entry & 0x7FF0000000000000ULL) != 0ULL ||
            ((entry & user_address::PTE_ADDRESS_MASK) & ~g_physical_mask) != 0ULL)
            return false;
        writable = writable && ((entry & PAGE_WRITABLE) != 0ULL);
        executable = executable && ((entry & PAGE_NX) == 0ULL);
        user = user || ((entry & PAGE_USER) != 0ULL);

        if ((level == 3u || level == 2u) && (entry & PAGE_HUGE) != 0ULL) {
            const uint32_t page_shift = level == 3u ? 30u : 21u;
            const uint64_t page_mask = (1ULL << page_shift) - 1ULL;
            const uint64_t reserved_low = page_mask & ~0x1FFFULL;
            if ((entry & reserved_low) != 0ULL) return false;
            const uint64_t physical_base =
                (entry & user_address::PTE_ADDRESS_MASK) & ~page_mask;
            out_info.physical_address = physical_base |
                (virtual_address & page_mask);
            if (out_info.physical_address == 0ULL ||
                (out_info.physical_address & ~g_physical_mask) != 0ULL) return false;
            out_info.permissions = KERNEL_MAPPING_READ |
                (writable ? KERNEL_MAPPING_WRITE : 0u) |
                (executable ? KERNEL_MAPPING_EXECUTE : 0u) |
                (user ? KERNEL_MAPPING_USER : 0u);
            out_info.page_size = static_cast<uint32_t>(1ULL << page_shift);
            return true;
        }
        if (level == 1u) {
            const uint64_t physical_base =
                entry & user_address::PTE_ADDRESS_MASK;
            if (physical_base == 0ULL) return false;
            out_info.physical_address = physical_base |
                (virtual_address & (user_address::PAGE_SIZE - 1ULL));
            out_info.permissions = KERNEL_MAPPING_READ |
                (writable ? KERNEL_MAPPING_WRITE : 0u) |
                (executable ? KERNEL_MAPPING_EXECUTE : 0u) |
                (user ? KERNEL_MAPPING_USER : 0u);
            out_info.page_size = static_cast<uint32_t>(user_address::PAGE_SIZE);
            return true;
        }
        if ((entry & PAGE_HUGE) != 0ULL) return false;
        const uint64_t child_physical = entry & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(child_physical) ||
            (child_physical & ~g_physical_mask) != 0ULL) return false;
        table = map_table(child_physical);
        if (!table) return false;
    }
    return false;
}

constexpr uint64_t PAGE_PAT_LARGE = 1ULL << 12;
constexpr uint64_t PAGE_PAT_4K = 1ULL << 7;
constexpr uint64_t PAGE_SOFTWARE_LOW = 7ULL << 9;
constexpr uint64_t HUGE_LEAF_LOW_FLAGS = 0x1FFFULL;
constexpr uint64_t IDENTITY_TABLE_PARENT_FLAGS =
    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_ACCESSED |
    PAGE_SOFTWARE_LOW | PAGE_NX;

bool identity_roots_share_slot_zero() {
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!kernel_root) return false;
    const uint64_t kernel_entry = kernel_root[0];
    if ((kernel_entry & PAGE_PRESENT) == 0ULL ||
        (kernel_entry & PAGE_HUGE) != 0ULL) return false;
    const uint64_t kernel_table = kernel_entry & user_address::PTE_ADDRESS_MASK;
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i) {
        if (!g_spaces[i].valid) continue;
        volatile uint64_t* root = map_table(g_spaces[i].root_physical);
        if (!root) return false;
        const uint64_t entry = root[0];
        if ((entry & PAGE_PRESENT) == 0ULL ||
            (entry & PAGE_HUGE) != 0ULL ||
            (entry & user_address::PTE_ADDRESS_MASK) != kernel_table ||
            (entry & ~PAGE_ACCESSED) != (kernel_entry & ~PAGE_ACCESSED))
            return false;
    }
    return true;
}

bool split_identity_1g(volatile uint64_t* pdpt, uint32_t index,
                       uint64_t physical_address) {
    if (!pdpt || index >= 512u) { g_identity_permission_failure_reason = "SPLIT1_BAD_PARENT"; return false; }
    const uint64_t old = pdpt[index];
    if ((old & (PAGE_PRESENT | PAGE_HUGE)) !=
            (PAGE_PRESENT | PAGE_HUGE) ||
        (old & 0x7FF0000000000000ULL) != 0ULL) { g_identity_permission_failure_reason = "SPLIT1_BAD_ENTRY"; return false; }
    const uint64_t page_mask = (1ULL << 30) - 1ULL;
    const uint64_t reserved_low = page_mask & ~0x1FFFULL;
    if ((old & reserved_low) != 0ULL) { g_identity_permission_failure_reason = "SPLIT1_RESERVED"; return false; }
    const uint64_t base =
        (old & user_address::PTE_ADDRESS_MASK) & ~page_mask;
    const uint64_t last = base + page_mask - (user_address::PAGE_SIZE - 1ULL);
    if (last < base) { g_identity_permission_failure_reason = "SPLIT1_OVERFLOW"; return false; }
    if ((last & ~g_physical_mask) != 0ULL) { g_identity_permission_failure_reason = "SPLIT1_PHYS_WIDTH"; return false; }
    if (physical_address < base || physical_address - base >= (1ULL << 30)) { g_identity_permission_failure_reason = "SPLIT1_TARGET_RANGE";
        return false; }

    uint64_t pd_physical = 0ULL;
    volatile uint64_t* pd = nullptr;
    if (!allocate_zeroed_table(pd_physical, pd)) { g_identity_permission_failure_reason = "SPLIT1_ALLOC"; return false; }
    const uint64_t large_flags = old & (HUGE_LEAF_LOW_FLAGS | PAGE_NX);
    for (uint32_t i = 0; i < 512u; ++i) {
        const uint64_t child_base = base + (static_cast<uint64_t>(i) << 21);
        if ((child_base & 0x1FFFFFULL) != 0ULL ||
            (child_base & ~g_physical_mask) != 0ULL) {
            if (g_platform.release_page) (void)g_platform.release_page(pd_physical);
            return false;
        }
        pd[i] = child_base | large_flags;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    pdpt[index] = pd_physical | (old & IDENTITY_TABLE_PARENT_FLAGS);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    invalidate_shared_kernel_page(physical_address);
    return true;
}

bool split_identity_2m(volatile uint64_t* pd, uint32_t index,
                       uint64_t physical_address) {
    if (!pd || index >= 512u) { g_identity_permission_failure_reason = "SPLIT2_BAD_PARENT"; return false; }
    const uint64_t old = pd[index];
    if ((old & (PAGE_PRESENT | PAGE_HUGE)) !=
            (PAGE_PRESENT | PAGE_HUGE) ||
        (old & 0x7FF0000000000000ULL) != 0ULL) { g_identity_permission_failure_reason = "SPLIT2_BAD_ENTRY"; return false; }
    const uint64_t page_mask = (1ULL << 21) - 1ULL;
    const uint64_t reserved_low = page_mask & ~0x1FFFULL;
    if ((old & reserved_low) != 0ULL) { g_identity_permission_failure_reason = "SPLIT2_RESERVED"; return false; }
    const uint64_t base =
        (old & user_address::PTE_ADDRESS_MASK) & ~page_mask;
    const uint64_t last = base + page_mask - (user_address::PAGE_SIZE - 1ULL);
    if (last < base) { g_identity_permission_failure_reason = "SPLIT2_OVERFLOW"; return false; }
    if ((last & ~g_physical_mask) != 0ULL) { g_identity_permission_failure_reason = "SPLIT2_PHYS_WIDTH"; return false; }
    if (physical_address < base || physical_address - base >= (1ULL << 21)) { g_identity_permission_failure_reason = "SPLIT2_TARGET_RANGE";
        return false; }

    uint64_t pt_physical = 0ULL;
    volatile uint64_t* pt = nullptr;
    if (!allocate_zeroed_table(pt_physical, pt)) { g_identity_permission_failure_reason = "SPLIT2_ALLOC"; return false; }
    const uint64_t large_flags = old & (HUGE_LEAF_LOW_FLAGS | PAGE_NX);
    uint64_t small_flags = large_flags & ~(PAGE_HUGE | PAGE_PAT_LARGE);
    if ((large_flags & PAGE_PAT_LARGE) != 0ULL)
        small_flags |= PAGE_PAT_4K;
    for (uint32_t i = 0; i < 512u; ++i) {
        const uint64_t child_base = base + (static_cast<uint64_t>(i) << 12);
        if ((child_base & (user_address::PAGE_SIZE - 1ULL)) != 0ULL ||
            (child_base & ~g_physical_mask) != 0ULL) {
            if (g_platform.release_page) (void)g_platform.release_page(pt_physical);
            g_identity_permission_failure_reason = "SPLIT2_CHILD_PHYS";
            return false;
        }
        pt[i] = child_base | small_flags;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    pd[index] = pt_physical | (old & IDENTITY_TABLE_PARENT_FLAGS);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    invalidate_shared_kernel_page(physical_address);
    return true;
}

bool walk_identity_page_leaf(uint64_t physical_address,
                             volatile uint64_t*& out_pt,
                             uint32_t& out_index,
                             bool& out_parent_writable) {
    out_pt = nullptr;
    out_index = 0u;
    out_parent_writable = false;
    if (physical_address >= ADDRESS_SPACE_KERNEL_LIMIT ||
        !valid_physical_page(physical_address)) return false;
    volatile uint64_t* root = map_table(g_kernel_root);
    if (!root) return false;
    const uint32_t pml4_index =
        static_cast<uint32_t>((physical_address >> 39) & PML4_INDEX_MASK);
    const uint32_t pdpt_index =
        static_cast<uint32_t>((physical_address >> 30) & PML4_INDEX_MASK);
    const uint32_t pd_index =
        static_cast<uint32_t>((physical_address >> 21) & PML4_INDEX_MASK);
    const uint32_t pt_index =
        static_cast<uint32_t>((physical_address >> 12) & PML4_INDEX_MASK);
    if (pml4_index != 0u) { g_identity_permission_failure_reason = "IDENTITY_PML4_INDEX"; return false; }

    const uint64_t pml4e = root[pml4_index];
    if ((pml4e & (PAGE_PRESENT | PAGE_USER | PAGE_HUGE)) != PAGE_PRESENT ||
        (pml4e & 0x7FF0000000000000ULL) != 0ULL) { g_identity_permission_failure_reason = "IDENTITY_PML4E"; return false; }
    volatile uint64_t* pdpt = map_table(
        pml4e & user_address::PTE_ADDRESS_MASK);
    if (!pdpt) { g_identity_permission_failure_reason = "IDENTITY_PDPT_MAP"; return false; }

    uint64_t pdpte = pdpt[pdpt_index];
    if ((pdpte & PAGE_PRESENT) == 0ULL) { g_identity_permission_failure_reason = "IDENTITY_PDPTE_ABSENT"; return false; }
    if ((pdpte & PAGE_HUGE) != 0ULL) {
        if (!split_identity_1g(pdpt, pdpt_index, physical_address)) return false;
        pdpte = pdpt[pdpt_index];
    }
    if ((pdpte & (PAGE_PRESENT | PAGE_USER | PAGE_HUGE)) != PAGE_PRESENT ||
        (pdpte & 0x7FF0000000000000ULL) != 0ULL) { g_identity_permission_failure_reason = "IDENTITY_PDPTE_INVALID"; return false; }
    volatile uint64_t* pd = map_table(
        pdpte & user_address::PTE_ADDRESS_MASK);
    if (!pd) { g_identity_permission_failure_reason = "IDENTITY_PD_MAP"; return false; }

    uint64_t pde = pd[pd_index];
    if ((pde & PAGE_PRESENT) == 0ULL) { g_identity_permission_failure_reason = "IDENTITY_PDE_ABSENT"; return false; }
    if ((pde & PAGE_HUGE) != 0ULL) {
        if (!split_identity_2m(pd, pd_index, physical_address)) return false;
        pde = pd[pd_index];
    }
    if ((pde & (PAGE_PRESENT | PAGE_USER | PAGE_HUGE)) != PAGE_PRESENT ||
        (pde & 0x7FF0000000000000ULL) != 0ULL) { g_identity_permission_failure_reason = "IDENTITY_PDE_INVALID"; return false; }
    volatile uint64_t* pt = map_table(
        pde & user_address::PTE_ADDRESS_MASK);
    if (!pt) { g_identity_permission_failure_reason = "IDENTITY_PT_MAP"; return false; }

    out_pt = pt;
    out_index = pt_index;
    out_parent_writable =
        (pml4e & PAGE_WRITABLE) != 0ULL &&
        (pdpte & PAGE_WRITABLE) != 0ULL &&
        (pde & PAGE_WRITABLE) != 0ULL;
    return true;
}

bool module_subtree_physical_alias(uint64_t physical_page) {
    volatile uint64_t* root = map_table(g_kernel_root);
    if (!root) return true;
    const uint64_t pml4e = root[KERNEL_MODULE_PML4_INDEX];
    if ((pml4e & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX | PAGE_HUGE)) !=
            (PAGE_PRESENT | PAGE_WRITABLE)) return true;
    volatile uint64_t* pdpt = map_table(
        pml4e & user_address::PTE_ADDRESS_MASK);
    if (!pdpt) return true;
    for (uint32_t i = 0; i < 512u; ++i) {
        const uint64_t pdpte = pdpt[i];
        if ((pdpte & PAGE_PRESENT) == 0ULL) {
            if (pdpte != 0ULL) return true;
            continue;
        }
        if ((pdpte & (PAGE_USER | PAGE_NX)) != 0ULL ||
            (pdpte & 0x7FF0000000000000ULL) != 0ULL ||
            ((pdpte & user_address::PTE_ADDRESS_MASK) & ~g_physical_mask) != 0ULL)
            return true;
        if ((pdpte & PAGE_HUGE) != 0ULL) {
            const uint64_t huge_mask = (1ULL << 30) - 1ULL;
            if ((pdpte & (huge_mask & ~0x1FFFULL)) != 0ULL) return true;
            const uint64_t base = (pdpte & user_address::PTE_ADDRESS_MASK) &
                                  ~huge_mask;
            if (physical_page >= base && physical_page - base < (1ULL << 30))
                return true;
            continue;
        }
        const uint64_t pd_phys = pdpte & user_address::PTE_ADDRESS_MASK;
        if (!valid_physical_page(pd_phys)) return true;
        volatile uint64_t* pd = map_table(pd_phys);
        if (!pd) return true;
        for (uint32_t j = 0; j < 512u; ++j) {
            const uint64_t pde = pd[j];
            if ((pde & PAGE_PRESENT) == 0ULL) {
                if (pde != 0ULL) return true;
                continue;
            }
            if ((pde & (PAGE_USER | PAGE_NX)) != 0ULL ||
                (pde & 0x7FF0000000000000ULL) != 0ULL ||
                ((pde & user_address::PTE_ADDRESS_MASK) & ~g_physical_mask) != 0ULL)
                return true;
            if ((pde & PAGE_HUGE) != 0ULL) {
                const uint64_t huge_mask = (1ULL << 21) - 1ULL;
                if ((pde & (huge_mask & ~0x1FFFULL)) != 0ULL) return true;
                const uint64_t base = (pde & user_address::PTE_ADDRESS_MASK) &
                                      ~huge_mask;
                if (physical_page >= base && physical_page - base < (1ULL << 21))
                    return true;
                continue;
            }
            const uint64_t pt_phys = pde & user_address::PTE_ADDRESS_MASK;
            if (!valid_physical_page(pt_phys)) return true;
            volatile uint64_t* pt = map_table(pt_phys);
            if (!pt) return true;
            for (uint32_t k = 0; k < 512u; ++k) {
                const uint64_t pte = pt[k];
                if ((pte & PAGE_PRESENT) == 0ULL) {
                    if (pte != 0ULL) return true;
                    continue;
                }
                if ((pte & 0x7FF0000000000000ULL) != 0ULL ||
                    ((pte & user_address::PTE_ADDRESS_MASK) & ~g_physical_mask) != 0ULL)
                    return true;
                if ((pte & user_address::PTE_ADDRESS_MASK) == physical_page)
                    return true;
            }
        }
    }
    return false;
}

} // namespace

bool address_space_manager_init(const AddressSpacePlatform& platform) {
    lock_spaces();
    g_ready = false;
    g_kernel_root = 0ULL;
    g_active_root = 0ULL;
    g_physical_mask = 0ULL;
    g_space_count = 0u;
    __atomic_store_n(&g_tlb_invalidation_generation, 0ULL, __ATOMIC_RELEASE);
    g_failure_reason = "PLATFORM_VALIDATION_FAILED";
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i)
        g_spaces[i] = AddressSpaceRecord{};
    for (uint32_t i = 0; i < KERNEL_MMIO_MAX_PAGES; ++i)
        g_mmio_records[i] = KernelMmioRecord{};

    if (platform.online_cpu_count == 0u) {
        g_failure_reason = "CPU_TOPOLOGY_UNAVAILABLE";
        unlock_spaces();
        return false;
    }
    if (platform.online_cpu_count != 1u) {
        g_failure_reason = "SMP_TLB_SHOOTDOWN_BACKEND_UNAVAILABLE";
        unlock_spaces();
        return false;
    }

    if (!platform.allocate_page || !platform.map_table ||
        !platform.critical_enter || !platform.critical_leave ||
        !platform.switch_root || !platform.invalidate_page ||
        platform.physical_address_bits < 32u ||
        platform.physical_address_bits > 52u ||
        (platform.cr0 & (CR0_PE | CR0_PG | CR0_WP)) !=
            (CR0_PE | CR0_PG | CR0_WP) ||
        (platform.cr4 & CR4_PAE) == 0ULL ||
        (platform.cr4 & (CR4_LA57 | CR4_PCIDE)) != 0ULL ||
        (platform.cr3 & 0xFE7ULL) != 0ULL ||
        (platform.efer & (EFER_LMA | EFER_NXE)) != (EFER_LMA | EFER_NXE)) {
        g_failure_reason = "CPU_OR_PLATFORM_REQUIREMENT_FAILED";
        unlock_spaces();
        return false;
    }

    const uint64_t physical_limit =
        (1ULL << platform.physical_address_bits) - 1ULL;
    const uint64_t physical_mask =
        physical_limit & user_address::PTE_ADDRESS_MASK;
    const uint64_t root_physical = platform.cr3 & user_address::PTE_ADDRESS_MASK;
    if ((platform.cr3 & ~(user_address::PTE_ADDRESS_MASK | 0xFFFULL)) != 0ULL ||
        root_physical == 0ULL || (root_physical & 0xFFFULL) != 0ULL ||
        (root_physical & ~physical_mask) != 0ULL) {
        g_failure_reason = "CR3_ROOT_INVALID";
        unlock_spaces();
        return false;
    }

    g_platform = platform;
    g_physical_mask = physical_mask;
    volatile uint64_t* kernel_pml4 = g_platform.map_table(root_physical);
    if (!kernel_pml4) {
        g_failure_reason = "CR3_ROOT_NOT_IDENTITY_ACCESSIBLE";
        g_platform = AddressSpacePlatform{};
        g_physical_mask = 0ULL;
        unlock_spaces();
        return false;
    }

    /* Kernel image is linked in low identity space; its PML4 entry must exist. */
    if ((kernel_pml4[0] & PAGE_PRESENT) == 0ULL ||
        kernel_pml4[1] != 0ULL ||
        kernel_pml4[KERNEL_MMIO_PML4_INDEX] != 0ULL ||
        kernel_pml4[KERNEL_MODULE_PML4_INDEX] != 0ULL ||
        kernel_pml4[KERNEL_STACK_PML4_INDEX] != 0ULL) {
        g_failure_reason = (kernel_pml4[1] != 0ULL)
            ? "USER_PML4_SLOT_1_OCCUPIED" :
            ((kernel_pml4[KERNEL_MMIO_PML4_INDEX] != 0ULL)
                ? "KERNEL_MMIO_PML4_SLOT_508_OCCUPIED" :
             ((kernel_pml4[KERNEL_MODULE_PML4_INDEX] != 0ULL)
                ? "KERNEL_MODULE_PML4_SLOT_509_OCCUPIED" :
             ((kernel_pml4[KERNEL_STACK_PML4_INDEX] != 0ULL)
                ? "KERNEL_STACK_PML4_SLOT_510_OCCUPIED"
                : "LOW_IDENTITY_PML4_SLOT_MISSING")));
        g_platform = AddressSpacePlatform{};
        g_physical_mask = 0ULL;
        unlock_spaces();
        return false;
    }

    /* Slot 509 is a shared supervisor module arena. Its upper-level entries
       are writable and executable-capable; per-page NX/RW policy is enforced
       only at the leaf. Slot 510 remains the separate writable-NX stack arena. */
    uint64_t module_pdpt_physical = 0ULL;
    volatile uint64_t* module_pdpt = nullptr;
    if (!allocate_zeroed_table(module_pdpt_physical, module_pdpt) ||
        !module_pdpt) {
        g_failure_reason = "MODULE_PAGE_TABLE_ALLOCATION_FAILED";
        g_platform = AddressSpacePlatform{};
        g_physical_mask = 0ULL;
        unlock_spaces();
        return false;
    }
    uint64_t stack_pdpt_physical = 0ULL;
    volatile uint64_t* stack_pdpt = nullptr;
    if (!allocate_zeroed_table(stack_pdpt_physical, stack_pdpt) ||
        !stack_pdpt) {
        if (g_platform.release_page)
            (void)g_platform.release_page(module_pdpt_physical);
        g_failure_reason = "STACK_PAGE_TABLE_ALLOCATION_FAILED";
        g_platform = AddressSpacePlatform{};
        g_physical_mask = 0ULL;
        unlock_spaces();
        return false;
    }
    uint64_t mmio_pdpt_physical = 0ULL;
    volatile uint64_t* mmio_pdpt = nullptr;
    if (!allocate_zeroed_table(mmio_pdpt_physical, mmio_pdpt) ||
        !mmio_pdpt) {
        if (g_platform.release_page) {
            (void)g_platform.release_page(stack_pdpt_physical);
            (void)g_platform.release_page(module_pdpt_physical);
        }
        g_failure_reason = "MMIO_PAGE_TABLE_ALLOCATION_FAILED";
        g_platform = AddressSpacePlatform{};
        g_physical_mask = 0ULL;
        unlock_spaces();
        return false;
    }

    kernel_pml4[KERNEL_MMIO_PML4_INDEX] =
        mmio_pdpt_physical | KERNEL_MMIO_TABLE_FLAGS;
    kernel_pml4[KERNEL_MODULE_PML4_INDEX] =
        module_pdpt_physical | KERNEL_MODULE_TABLE_FLAGS;
    kernel_pml4[KERNEL_STACK_PML4_INDEX] =
        stack_pdpt_physical | KERNEL_STACK_PTE_FLAGS;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    g_kernel_root = root_physical;
    g_active_root = root_physical;
    g_ready = true;
    g_failure_reason = "PASS";
    unlock_spaces();
    return true;
}

const char* address_space_failure_reason() { return g_failure_reason; }

bool address_space_manager_ready() {
    lock_spaces();
    const bool ready = g_ready;
    unlock_spaces();
    return ready;
}

uint32_t address_space_online_cpu_count() {
    lock_spaces();
    const uint32_t count = g_ready ? g_platform.online_cpu_count : 0u;
    unlock_spaces();
    return count;
}

uint64_t address_space_tlb_invalidation_generation() {
    return __atomic_load_n(&g_tlb_invalidation_generation, __ATOMIC_ACQUIRE);
}

uint64_t address_space_kernel_root() {
    lock_spaces();
    const uint64_t root = g_ready ? g_kernel_root : 0ULL;
    unlock_spaces();
    return root;
}

uint64_t address_space_active_root() {
    lock_spaces();
    const uint64_t root = g_ready ? g_active_root : 0ULL;
    unlock_spaces();
    return root;
}

static bool activate_root(const AddressSpace* candidate, bool kernel_root) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return false;
    const uint64_t saved_flags = g_platform.critical_enter();
    lock_spaces();

    const uint64_t target_root = kernel_root
        ? g_kernel_root
        : (candidate ? candidate->cr3 : 0ULL);
    const bool target_valid = kernel_root ||
        (candidate && candidate->magic == ADDRESS_SPACE_MAGIC &&
         is_registered_space(*candidate));

    bool switched = false;
    if (g_ready && target_valid && valid_physical_page(target_root)) {
        if (target_root == g_active_root) {
            switched = true;
        } else if (g_platform.switch_root(g_active_root, target_root)) {
            g_active_root = target_root;
            switched = true;
        }
    }

    unlock_spaces();
    g_platform.critical_leave(saved_flags);
    return switched;
}

bool address_space_activate(const AddressSpace& space) {
    return activate_root(&space, false);
}

bool address_space_activate_kernel() {
    return activate_root(nullptr, true);
}

bool address_space_root_registered(uint64_t root_physical) {
    lock_spaces();
    bool found = false;
    if (g_ready && root_physical != 0ULL &&
        (root_physical & 0xFFFULL) == 0ULL &&
        root_physical != g_kernel_root &&
        (root_physical & ~g_physical_mask) == 0ULL) {
        for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i) {
            if (g_spaces[i].valid &&
                g_spaces[i].root_physical == root_physical) {
                found = true;
                break;
            }
        }
    }
    unlock_spaces();
    return found;
}

bool address_space_activate_root(uint64_t root_physical) {
    AddressSpace candidate{};
    candidate.cr3 = root_physical;
    candidate.mapped_pages = 0u;
    candidate.magic = ADDRESS_SPACE_MAGIC;
    return activate_root(&candidate, false);
}

bool address_space_create(AddressSpace& out_space) {
    out_space = AddressSpace{};
    lock_spaces();
    if (!g_ready || g_space_count >= ADDRESS_SPACE_MAX) {
        unlock_spaces();
        return false;
    }

    uint32_t free_slot = ADDRESS_SPACE_MAX;
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i) {
        if (!g_spaces[i].valid) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == ADDRESS_SPACE_MAX) {
        unlock_spaces();
        return false;
    }

    const uint64_t new_root_physical = g_platform.allocate_page();
    if (!valid_physical_page(new_root_physical) ||
        new_root_physical == g_kernel_root) {
        unlock_spaces();
        return false;
    }
    volatile uint64_t* new_root = map_table(new_root_physical);
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!new_root || !kernel_root) {
        if (g_platform.release_page)
            (void)g_platform.release_page(new_root_physical);
        unlock_spaces();
        return false;
    }

    for (uint32_t i = 0; i < 512u; ++i)
        new_root[i] = kernel_root[i] & ~PAGE_USER;
    /* The manager accepted only an empty user slot, but assert the clone too. */
    if (new_root[1] != 0ULL) {
        if (g_platform.release_page)
            (void)g_platform.release_page(new_root_physical);
        unlock_spaces();
        return false;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);

    g_spaces[free_slot].root_physical = new_root_physical;
    g_spaces[free_slot].valid = true;
    ++g_space_count;

    out_space.cr3 = new_root_physical;
    out_space.mapped_pages = 0u;
    out_space.magic = ADDRESS_SPACE_MAGIC;
    unlock_spaces();
    return true;
}

bool address_space_destroy(AddressSpace& space) {
    lock_spaces();
    if (!g_ready || !g_platform.release_page ||
        !is_registered_space(space) || space.cr3 == g_active_root ||
        space.mapped_pages != 0u) {
        unlock_spaces();
        return false;
    }

    volatile uint64_t* root = map_table(space.cr3);
    volatile uint64_t* kernel_root = map_table(g_kernel_root);
    if (!root || !kernel_root || root[1] != 0ULL) {
        unlock_spaces();
        return false;
    }
    /* Refuse to free a root that carries private supervisor mappings or
       differs from the manager's shared kernel template. */
    for (uint32_t i = 0u; i < 512u; ++i) {
        if (i == 1u) continue;
        if (root[i] != (kernel_root[i] & ~PAGE_USER)) {
            unlock_spaces();
            return false;
        }
    }

    uint32_t slot = ADDRESS_SPACE_MAX;
    for (uint32_t i = 0u; i < ADDRESS_SPACE_MAX; ++i) {
        if (g_spaces[i].valid && g_spaces[i].root_physical == space.cr3) {
            slot = i;
            break;
        }
    }
    if (slot == ADDRESS_SPACE_MAX ||
        !g_platform.release_page(space.cr3)) {
        unlock_spaces();
        return false;
    }

    g_spaces[slot] = AddressSpaceRecord{};
    if (g_space_count != 0u) --g_space_count;
    space = AddressSpace{};
    unlock_spaces();
    return true;
}

bool address_space_map_user_page(AddressSpace& space,
                                 uint64_t virtual_address,
                                 uint64_t physical_address,
                                 uint32_t permissions) {
    using namespace user_address;
    lock_spaces();
    if (!g_ready || !is_registered_space(space) ||
        space.cr3 == g_active_root ||
        !is_user_address(virtual_address) || !page_aligned(virtual_address) ||
        !valid_physical_page(physical_address) ||
        space.mapped_pages == 0xFFFFFFFFu) {
        unlock_spaces();
        return false;
    }

    uint64_t leaf_entry = 0ULL;
    if (!build_user_pte(physical_address, permissions, true, leaf_entry) ||
        (physical_address & ~g_physical_mask) != 0ULL) {
        unlock_spaces();
        return false;
    }

    volatile uint64_t* leaf_table = nullptr;
    uint32_t leaf_index = 0;
    if (!walk_to_leaf(space, virtual_address, leaf_table, leaf_index) ||
        !leaf_table || leaf_table[leaf_index] != 0ULL) {
        prune_empty_user_path(space, virtual_address);
        unlock_spaces();
        return false;
    }

    leaf_table[leaf_index] = leaf_entry;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ++space.mapped_pages;
    unlock_spaces();
    return true;
}

bool address_space_unmap_user_page(AddressSpace& space,
                                   uint64_t virtual_address) {
    lock_spaces();
    if (!g_ready || !is_registered_space(space) ||
        space.cr3 == g_active_root ||
        !user_address::is_user_address(virtual_address) ||
        !user_address::page_aligned(virtual_address)) {
        unlock_spaces();
        return false;
    }

    volatile uint64_t* leaf_table = nullptr;
    uint32_t leaf_index = 0;
    if (!walk_existing_leaf(space, virtual_address, leaf_table, leaf_index) ||
        !leaf_table || (leaf_table[leaf_index] & PAGE_PRESENT) == 0ULL ||
        (leaf_table[leaf_index] & PAGE_USER) == 0ULL ||
        space.mapped_pages == 0u) {
        unlock_spaces();
        return false;
    }

    leaf_table[leaf_index] = 0ULL;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    --space.mapped_pages;
    prune_empty_user_path(space, virtual_address);
    unlock_spaces();
    return true;
}

bool address_space_translate_user(const AddressSpace& space,
                                  uint64_t virtual_address,
                                  uint64_t& out_physical_address,
                                  uint32_t& out_permissions) {
    using namespace user_address;
    out_physical_address = 0ULL;
    out_permissions = 0u;
    lock_spaces();
    if (!g_ready || !is_registered_space(space) ||
        !is_user_address(virtual_address)) {
        unlock_spaces();
        return false;
    }

    volatile uint64_t* leaf_table = nullptr;
    uint32_t leaf_index = 0;
    if (!walk_existing_leaf(space, virtual_address, leaf_table, leaf_index) ||
        !leaf_table) {
        unlock_spaces();
        return false;
    }

    const uint64_t leaf = leaf_table[leaf_index];
    const bool leaf_writable = (leaf & PAGE_WRITABLE) != 0ULL;
    const bool leaf_executable = (leaf & PTE_NX) == 0ULL;
    const uint64_t leaf_physical = leaf & PTE_ADDRESS_MASK;
    if ((leaf & (PAGE_PRESENT | PAGE_USER)) !=
            (PAGE_PRESENT | PAGE_USER) ||
        !valid_physical_page(leaf_physical) ||
        (leaf_physical & ~g_physical_mask) != 0ULL ||
        (leaf_writable && leaf_executable)) {
        unlock_spaces();
        return false;
    }

    out_physical_address =
        (leaf & PTE_ADDRESS_MASK) | (virtual_address & (user_address::PAGE_SIZE - 1ULL));
    out_permissions = USER_PAGE_READ;
    if (leaf_writable) out_permissions |= USER_PAGE_WRITE;
    if (leaf_executable) out_permissions |= USER_PAGE_EXEC;
    unlock_spaces();
    return true;
}

bool address_space_map_kernel_stack_range(
    uint64_t virtual_base, uint64_t physical_base, uint32_t page_count
) {
    using namespace user_address;
    if (page_count == 0u || page_count > KERNEL_STACK_MAX_MAP_PAGES ||
        !page_aligned(virtual_base) || !page_aligned(physical_base) ||
        virtual_base < KERNEL_STACK_REGION_BASE + PAGE_SIZE ||
        virtual_base >= KERNEL_STACK_REGION_END ||
        page_count > ((KERNEL_STACK_REGION_END - virtual_base) / PAGE_SIZE) ||
        virtual_base + static_cast<uint64_t>(page_count) * PAGE_SIZE >
            KERNEL_STACK_REGION_END - PAGE_SIZE ||
        physical_base == 0ULL ||
        static_cast<uint64_t>(page_count) * PAGE_SIZE >
            (~0ULL - physical_base) || !g_platform.invalidate_page) return false;

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_spaces();
    bool ok = g_ready;
    volatile uint64_t* root = nullptr;
    if (ok) ok = kernel_stack_root_table(g_kernel_root, root);

    /* Resolve every leaf first. Empty intermediate page tables may be created,
       but no stack page becomes visible unless the entire mapping can commit. */
    volatile uint64_t* leaves[KERNEL_STACK_MAX_MAP_PAGES]{};
    uint32_t leaf_indices[KERNEL_STACK_MAX_MAP_PAGES]{};
    if (ok) {
        const uint64_t low_guard = virtual_base - PAGE_SIZE;
        const uint64_t high_guard = virtual_base +
            static_cast<uint64_t>(page_count) * PAGE_SIZE;
        uint64_t entry = 0ULL;
        if (!walk_kernel_stack_leaf_readonly(root, low_guard, entry) || entry != 0ULL ||
            !walk_kernel_stack_leaf_readonly(root, high_guard, entry) || entry != 0ULL) {
            ok = false;
        }
    }
    for (uint32_t i = 0; ok && i < page_count; ++i) {
        const uint64_t va = virtual_base + static_cast<uint64_t>(i) * PAGE_SIZE;
        if (!walk_kernel_stack_leaf(root, va, true, leaves[i], leaf_indices[i]) ||
            !leaves[i] || leaves[i][leaf_indices[i]] != 0ULL) {
            ok = false;
        }
    }

    if (ok) {
        for (uint32_t i = 0; i < page_count; ++i) {
            const uint64_t pa = physical_base + static_cast<uint64_t>(i) * PAGE_SIZE;
            if (!valid_physical_page(pa) || (pa & ~g_physical_mask) != 0ULL) {
                ok = false;
                break;
            }
            leaves[i][leaf_indices[i]] = pa | KERNEL_STACK_PTE_FLAGS;
        }
        if (ok) {
            __atomic_thread_fence(__ATOMIC_RELEASE);
            for (uint32_t i = 0; i < page_count; ++i)
                invalidate_shared_kernel_page(
                    virtual_base + static_cast<uint64_t>(i) * PAGE_SIZE);
        } else {
            for (uint32_t i = 0; i < page_count; ++i)
                if (leaves[i]) leaves[i][leaf_indices[i]] = 0ULL;
        }
    }
    unlock_spaces();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return ok;
}

bool address_space_unmap_kernel_stack_range(
    uint64_t virtual_base, uint64_t physical_base, uint32_t page_count
) {
    using namespace user_address;
    if (page_count == 0u || page_count > KERNEL_STACK_MAX_MAP_PAGES ||
        !page_aligned(virtual_base) || !page_aligned(physical_base) ||
        virtual_base < KERNEL_STACK_REGION_BASE + PAGE_SIZE ||
        virtual_base >= KERNEL_STACK_REGION_END ||
        page_count > ((KERNEL_STACK_REGION_END - virtual_base) / PAGE_SIZE) ||
        virtual_base + static_cast<uint64_t>(page_count) * PAGE_SIZE >
            KERNEL_STACK_REGION_END - PAGE_SIZE ||
        physical_base == 0ULL ||
        static_cast<uint64_t>(page_count) * PAGE_SIZE > (~0ULL - physical_base) ||
        !g_platform.invalidate_page) return false;

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_stack_root_table(g_kernel_root, root);
    /* Preflight and retain every leaf pointer before mutating any PTE. */
    volatile uint64_t* leaves[KERNEL_STACK_MAX_MAP_PAGES]{};
    uint32_t leaf_indices[KERNEL_STACK_MAX_MAP_PAGES]{};
    for (uint32_t i = 0u; ok && i < page_count; ++i) {
        const uint64_t va = virtual_base + static_cast<uint64_t>(i) * PAGE_SIZE;
        const uint64_t pa = physical_base + static_cast<uint64_t>(i) * PAGE_SIZE;
        uint64_t entry = 0ULL;
        ok = walk_kernel_stack_leaf_readonly(root, va, entry) &&
            walk_kernel_stack_leaf(root, va, false, leaves[i], leaf_indices[i]) &&
            leaves[i] &&
            (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | user_address::PTE_NX)) ==
                (PAGE_PRESENT | PAGE_WRITABLE | user_address::PTE_NX) &&
            (entry & user_address::PTE_ADDRESS_MASK) == pa &&
            (pa & ~g_physical_mask) == 0ULL;
    }
    if (ok) {
        for (uint32_t i = 0u; i < page_count; ++i)
            leaves[i][leaf_indices[i]] = 0ULL;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (uint32_t i = 0u; i < page_count; ++i)
            invalidate_shared_kernel_page(
                virtual_base + static_cast<uint64_t>(i) * PAGE_SIZE);
    }
    unlock_spaces();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return ok;
}

bool address_space_kernel_stack_range_mapped(
    uint64_t root_physical, uint64_t virtual_base,
    uint64_t physical_base, uint32_t page_count
) {
    using namespace user_address;
    g_stack_mapping_failure_reason = "STACK_MAPPING_INVALID_ARGUMENT";
    if (page_count == 0u || page_count > KERNEL_STACK_MAX_MAP_PAGES ||
        !page_aligned(virtual_base) || !page_aligned(physical_base) ||
        virtual_base < KERNEL_STACK_REGION_BASE + PAGE_SIZE ||
        virtual_base >= KERNEL_STACK_REGION_END ||
        page_count > ((KERNEL_STACK_REGION_END - virtual_base) / PAGE_SIZE) ||
        virtual_base + static_cast<uint64_t>(page_count) * PAGE_SIZE >
            KERNEL_STACK_REGION_END - PAGE_SIZE ||
        physical_base == 0ULL ||
        static_cast<uint64_t>(page_count) * PAGE_SIZE >
            (~0ULL - physical_base)) return false;

    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_stack_root_table(root_physical, root);
    if (!g_ready) g_stack_mapping_failure_reason = "ADDRESS_SPACE_MANAGER_UNAVAILABLE";
    for (uint32_t i = 0; ok && i < page_count; ++i) {
        uint64_t entry = 0ULL;
        const uint64_t va = virtual_base + static_cast<uint64_t>(i) * PAGE_SIZE;
        const uint64_t expected_pa = physical_base +
            static_cast<uint64_t>(i) * PAGE_SIZE;
        if (!walk_kernel_stack_leaf_readonly(root, va, entry)) {
            g_stack_mapping_failure_reason = "STACK_LEAF_WALK_REJECTED";
            ok = false;
        } else if ((entry & PAGE_PRESENT) == 0ULL) {
            g_stack_mapping_failure_reason = "STACK_LEAF_NOT_PRESENT";
            ok = false;
        } else if ((entry & (PAGE_WRITABLE | user_address::PTE_NX | PAGE_USER)) !=
                   (PAGE_WRITABLE | user_address::PTE_NX)) {
            g_stack_mapping_failure_reason = "STACK_LEAF_PERMISSION_MISMATCH";
            ok = false;
        } else if ((entry & user_address::PTE_ADDRESS_MASK) != expected_pa) {
            g_stack_mapping_failure_reason = "STACK_LEAF_PHYSICAL_MISMATCH";
            ok = false;
        } else if ((expected_pa & ~g_physical_mask) != 0ULL) {
            g_stack_mapping_failure_reason = "STACK_LEAF_PHYSICAL_WIDTH_INVALID";
            ok = false;
        }
    }
    if (ok) g_stack_mapping_failure_reason = "PASS";
    unlock_spaces();
    return ok;
}

const char* address_space_kernel_stack_mapping_reason() {
    return g_stack_mapping_failure_reason;
}

bool address_space_kernel_stack_guard_unmapped(
    uint64_t root_physical, uint64_t guard_page_address
) {
    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_stack_root_table(root_physical, root) &&
        valid_kernel_stack_page_address(guard_page_address);
    uint64_t entry = 0ULL;
    if (ok) ok = walk_kernel_stack_leaf_readonly(root, guard_page_address, entry) &&
                 entry == 0ULL;
    unlock_spaces();
    return ok;
}


bool address_space_set_kernel_identity_page_permissions(
    uint64_t physical_address, uint32_t expected_permissions,
    uint32_t target_permissions
) {
    g_identity_permission_failure_reason = "INVALID_ARGUMENT";
    constexpr uint32_t allowed = KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE;
    if (!valid_physical_page(physical_address) ||
        physical_address >= ADDRESS_SPACE_KERNEL_LIMIT ||
        (expected_permissions & ~allowed) != 0u ||
        (target_permissions & KERNEL_MAPPING_EXECUTE) != 0u ||
        (target_permissions & ~KERNEL_MAPPING_WRITE) != 0u ||
        !g_platform.invalidate_page) return false;

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_spaces();
    bool ok = g_ready && identity_roots_share_slot_zero();
    if (!g_ready) g_identity_permission_failure_reason = "ADDRESS_SPACE_MANAGER_UNAVAILABLE";
    else if (!ok) g_identity_permission_failure_reason = "IDENTITY_SUBTREE_NOT_SHARED";

    KernelMappingInfo current{};
    if (ok && (!query_kernel_mapping_unlocked(
            g_kernel_root, physical_address, current) ||
        current.physical_address != physical_address ||
        (current.permissions & KERNEL_MAPPING_USER) != 0u)) {
        g_identity_permission_failure_reason = "IDENTITY_MAPPING_NOT_VALID";
        ok = false;
    }
    const uint32_t current_permissions = ok
        ? current.permissions & allowed : 0u;
    if (ok && current_permissions != expected_permissions) {
        g_identity_permission_failure_reason = "EXPECTED_PERMISSIONS_MISMATCH";
        ok = false;
    }
    if (ok && module_subtree_physical_alias(physical_address)) {
        g_identity_permission_failure_reason = "MODULE_ALIAS_STILL_PRESENT";
        ok = false;
    }

    volatile uint64_t* pt = nullptr;
    uint32_t pt_index = 0u;
    bool parents_writable = false;
    if (ok) g_identity_permission_failure_reason = "IDENTITY_WALK_IN_PROGRESS";
    if (ok && !walk_identity_page_leaf(physical_address, pt,
            pt_index, parents_writable)) {
        ok = false;
    }
    uint64_t old_leaf = 0ULL;
    if (ok) {
        old_leaf = pt[pt_index];
        if ((old_leaf & PAGE_PRESENT) == 0ULL ||
            (old_leaf & user_address::PTE_ADDRESS_MASK) != physical_address ||
            (old_leaf & PAGE_USER) != 0ULL) {
            g_identity_permission_failure_reason = "IDENTITY_LEAF_MISMATCH";
            ok = false;
        }
    }
    const bool target_writable =
        (target_permissions & KERNEL_MAPPING_WRITE) != 0u;
    if (ok && target_writable && !parents_writable) {
        g_identity_permission_failure_reason = "IDENTITY_PARENT_NOT_WRITABLE";
        ok = false;
    }
    if (ok) {
        uint64_t new_leaf = old_leaf | PAGE_NX;
        if (target_writable) new_leaf |= PAGE_WRITABLE;
        else new_leaf &= ~PAGE_WRITABLE;
        pt[pt_index] = new_leaf;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        invalidate_shared_kernel_page(physical_address);

        KernelMappingInfo verified{};
        if (!query_kernel_mapping_unlocked(g_kernel_root,
                physical_address, verified) ||
            verified.physical_address != physical_address ||
            (verified.permissions & KERNEL_MAPPING_USER) != 0u ||
            (verified.permissions & allowed) != target_permissions) {
            pt[pt_index] = old_leaf;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            invalidate_shared_kernel_page(physical_address);
            g_identity_permission_failure_reason = "POST_UPDATE_VERIFICATION_FAILED";
            ok = false;
        } else {
            g_identity_permission_failure_reason = "PASS";
        }
    }
    unlock_spaces();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return ok;
}

const char* address_space_kernel_identity_permission_reason() {
    return g_identity_permission_failure_reason;
}

bool address_space_query_kernel_mapping(
    uint64_t root_physical, uint64_t virtual_address,
    KernelMappingInfo& out_info
) {
    out_info = KernelMappingInfo{};
    lock_spaces();
    const bool ok = g_ready && query_kernel_mapping_unlocked(
        root_physical, virtual_address, out_info);
    unlock_spaces();
    return ok;
}

bool address_space_map_kernel_module_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint32_t mapping_flags
) {
    g_module_mapping_failure_reason = "INVALID_ARGUMENT";
    if (!valid_kernel_module_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        (physical_address & ~g_physical_mask) != 0ULL ||
        (mapping_flags & ~KERNEL_MODULE_MAP_FLAGS_ALL) != 0u ||
        (mapping_flags & KERNEL_MODULE_MAP_FLAGS_ALL) ==
            KERNEL_MODULE_MAP_FLAGS_ALL ||
        !g_platform.invalidate_page) return false;

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_spaces();
    bool ok = g_ready;
    volatile uint64_t* root = nullptr;
    if (ok) ok = kernel_module_root_table(g_kernel_root, root);

    KernelMappingInfo direct_alias{};
    if (ok && (!query_kernel_mapping_unlocked(
            g_kernel_root, physical_address, direct_alias) ||
        direct_alias.physical_address != physical_address ||
        (direct_alias.permissions & KERNEL_MAPPING_USER) != 0u)) {
        g_module_mapping_failure_reason = "IDENTITY_ALIAS_NOT_VALID";
        ok = false;
    }
    const bool wants_write = (mapping_flags & KERNEL_MODULE_MAP_WRITE) != 0u;
    const bool wants_execute = (mapping_flags & KERNEL_MODULE_MAP_EXECUTE) != 0u;
    const bool alias_write = (direct_alias.permissions & KERNEL_MAPPING_WRITE) != 0u;
    const bool alias_execute = (direct_alias.permissions & KERNEL_MAPPING_EXECUTE) != 0u;
    if (ok && ((alias_write && alias_execute) ||
        (wants_write && alias_execute) || (wants_execute && alias_write))) {
        g_module_mapping_failure_reason = "IDENTITY_ALIAS_WX_CONFLICT";
        ok = false;
    }
    if (ok && module_subtree_physical_alias(physical_address)) {
        g_module_mapping_failure_reason = "PHYSICAL_PAGE_ALREADY_MAPPED_IN_MODULE_ARENA";
        ok = false;
    }

    for (uint32_t i = 0u; ok && i < KERNEL_MMIO_MAX_PAGES; ++i) {
        const KernelMmioRecord& mmio = g_mmio_records[i];
        if (mmio.active && mmio.physical_address == physical_address) {
            g_module_mapping_failure_reason =
                "PHYSICAL_PAGE_ALREADY_OWNED_BY_MMIO";
            ok = false;
        }
    }

    volatile uint64_t* existing_leaf = nullptr;
    uint32_t existing_index = 0u;
    if (ok) {
        uint64_t existing_entry = 0ULL;
        if (!walk_kernel_module_leaf_readonly(root, virtual_address, existing_entry) ||
            existing_entry != 0ULL) {
            g_module_mapping_failure_reason = "VIRTUAL_PAGE_ALREADY_MAPPED_OR_INVALID";
            ok = false;
        }
    }
    if (ok && (!walk_kernel_module_leaf(root, virtual_address, true,
            existing_leaf, existing_index) || !existing_leaf ||
            existing_leaf[existing_index] != 0ULL)) {
        g_module_mapping_failure_reason = "MODULE_PAGE_TABLE_ALLOCATION_FAILED";
        ok = false;
    }
    if (ok) {
        uint64_t leaf_flags = PAGE_PRESENT;
        if (wants_write) leaf_flags |= PAGE_WRITABLE;
        if (!wants_execute) leaf_flags |= PAGE_NX;
        existing_leaf[existing_index] = physical_address | leaf_flags;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        invalidate_shared_kernel_page(virtual_address);
        g_module_mapping_failure_reason = "PASS";
    }
    unlock_spaces();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return ok;
}

bool address_space_unmap_kernel_module_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint32_t mapping_flags
) {
    g_module_mapping_failure_reason = "INVALID_ARGUMENT";
    if (!valid_kernel_module_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        (mapping_flags & ~KERNEL_MODULE_MAP_FLAGS_ALL) != 0u ||
        (mapping_flags & KERNEL_MODULE_MAP_FLAGS_ALL) ==
            KERNEL_MODULE_MAP_FLAGS_ALL ||
        !g_platform.invalidate_page) return false;

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_module_root_table(g_kernel_root, root);
    volatile uint64_t* leaf = nullptr;
    uint32_t leaf_index = 0u;
    if (ok) ok = walk_kernel_module_leaf(root, virtual_address, false,
                                         leaf, leaf_index) && leaf;
    const uint64_t entry = ok ? leaf[leaf_index] : 0ULL;
    const bool wants_write = (mapping_flags & KERNEL_MODULE_MAP_WRITE) != 0u;
    const bool wants_execute = (mapping_flags & KERNEL_MODULE_MAP_EXECUTE) != 0u;
    const uint64_t expected_flags = PAGE_PRESENT |
        (wants_write ? PAGE_WRITABLE : 0ULL) |
        (wants_execute ? 0ULL : PAGE_NX);
    if (!ok || (entry & user_address::PTE_ADDRESS_MASK) != physical_address ||
        (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX | PAGE_USER)) !=
            expected_flags) {
        g_module_mapping_failure_reason = "EXACT_MAPPING_NOT_FOUND";
        ok = false;
    }
    if (ok) {
        leaf[leaf_index] = 0ULL;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        invalidate_shared_kernel_page(virtual_address);
        g_module_mapping_failure_reason = "PASS";
    }
    unlock_spaces();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return ok;
}

bool address_space_kernel_module_page_mapped(
    uint64_t root_physical, uint64_t virtual_address,
    uint64_t physical_address, uint32_t mapping_flags
) {
    if (!valid_kernel_module_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        (mapping_flags & ~KERNEL_MODULE_MAP_FLAGS_ALL) != 0u ||
        (mapping_flags & KERNEL_MODULE_MAP_FLAGS_ALL) ==
            KERNEL_MODULE_MAP_FLAGS_ALL) return false;
    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_module_root_table(root_physical, root);
    uint64_t entry = 0ULL;
    if (ok) ok = walk_kernel_module_leaf_readonly(root,
        virtual_address, entry) && (entry & PAGE_PRESENT) != 0ULL;
    const bool wants_write = (mapping_flags & KERNEL_MODULE_MAP_WRITE) != 0u;
    const bool wants_execute = (mapping_flags & KERNEL_MODULE_MAP_EXECUTE) != 0u;
    const uint64_t expected_flags = PAGE_PRESENT |
        (wants_write ? PAGE_WRITABLE : 0ULL) |
        (wants_execute ? 0ULL : PAGE_NX);
    if (ok) ok = (entry & user_address::PTE_ADDRESS_MASK) == physical_address &&
        (entry & (PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX | PAGE_USER)) ==
            expected_flags;
    unlock_spaces();
    return ok;
}

bool address_space_kernel_module_guard_unmapped(
    uint64_t root_physical, uint64_t guard_page_address
) {
    if (!valid_kernel_module_region_page_address(guard_page_address))
        return false;
    lock_spaces();
    volatile uint64_t* root = nullptr;
    bool ok = g_ready && kernel_module_root_table(root_physical, root);
    uint64_t entry = 0ULL;
    if (ok) ok = walk_kernel_module_leaf_readonly(
        root, guard_page_address, entry) && entry == 0ULL;
    unlock_spaces();
    return ok;
}

const char* address_space_kernel_module_mapping_reason() {
    return g_module_mapping_failure_reason;
}


bool address_space_map_kernel_mmio_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint64_t cache_flags
) {
    g_mmio_mapping_failure_reason = "INVALID_ARGUMENT";

    constexpr uint64_t cache_mask =
        PAGE_PWT | PAGE_PCD | PAGE_PAT_4K;

    if (!valid_kernel_mmio_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        physical_address >= ADDRESS_SPACE_KERNEL_LIMIT ||
        (physical_address & ~g_physical_mask) != 0ULL ||
        (cache_flags & ~cache_mask) != 0ULL ||
        !g_platform.invalidate_page) {
        return false;
    }

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;

    lock_spaces();

    bool ok = g_ready;
    volatile uint64_t* root = nullptr;

    if (ok && !identity_roots_share_slot_zero()) {
        g_mmio_mapping_failure_reason =
            "IDENTITY_SUBTREE_NOT_SHARED";
        ok = false;
    }

    if (ok && module_subtree_physical_alias(physical_address)) {
        g_mmio_mapping_failure_reason =
            "PHYSICAL_PAGE_HAS_MODULE_ALIAS";
        ok = false;
    }

    if (ok) ok = kernel_mmio_root_table(g_kernel_root, root);

    uint32_t free_record = KERNEL_MMIO_MAX_PAGES;

    for (uint32_t i = 0; ok && i < KERNEL_MMIO_MAX_PAGES; ++i) {
        const KernelMmioRecord& record = g_mmio_records[i];
        if (!record.active) {
            if (free_record == KERNEL_MMIO_MAX_PAGES)
                free_record = i;
            continue;
        }

        if (record.virtual_address == virtual_address) {
            g_mmio_mapping_failure_reason = "VIRTUAL_ADDRESS_ALREADY_MAPPED";
            ok = false;
        } else if (record.physical_address == physical_address) {
            g_mmio_mapping_failure_reason = "PHYSICAL_PAGE_ALREADY_MAPPED";
            ok = false;
        }
    }

    if (ok && free_record == KERNEL_MMIO_MAX_PAGES) {
        g_mmio_mapping_failure_reason = "MMIO_REGISTRY_FULL";
        ok = false;
    }

    uint64_t existing_entry = 0ULL;
    if (ok && (!walk_kernel_mmio_leaf_readonly(
            root, virtual_address, existing_entry) ||
        existing_entry != 0ULL)) {
        g_mmio_mapping_failure_reason =
            "MMIO_VIRTUAL_PAGE_ALREADY_OCCUPIED";
        ok = false;
    }

    volatile uint64_t* mmio_leaf = nullptr;
    uint32_t mmio_leaf_index = 0u;

    if (ok && !walk_kernel_mmio_leaf(
            root, virtual_address, true,
            mmio_leaf, mmio_leaf_index)) {
        g_mmio_mapping_failure_reason =
            "MMIO_PAGE_TABLE_ALLOCATION_FAILED";
        ok = false;
    }

    if (ok && mmio_leaf[mmio_leaf_index] != 0ULL) {
        g_mmio_mapping_failure_reason =
            "MMIO_LEAF_NOT_EMPTY";
        ok = false;
    }

    volatile uint64_t* identity_pt = nullptr;
    uint32_t identity_index = 0u;
    bool identity_parents_writable = false;

    if (ok && !walk_identity_page_leaf(
            physical_address, identity_pt,
            identity_index, identity_parents_writable)) {
        g_mmio_mapping_failure_reason =
            "IDENTITY_ALIAS_NOT_MAPPED_OR_INVALID";
        ok = false;
    }

    uint64_t old_identity_entry = 0ULL;
    uint64_t old_identity_cache_flags = 0ULL;

    if (ok) {
        old_identity_entry = identity_pt[identity_index];

        if ((old_identity_entry & PAGE_PRESENT) == 0ULL ||
            (old_identity_entry & PAGE_USER) != 0ULL ||
            (old_identity_entry & user_address::PTE_ADDRESS_MASK) !=
                physical_address) {
            g_mmio_mapping_failure_reason =
                "IDENTITY_ALIAS_LEAF_MISMATCH";
            ok = false;
        } else {
            old_identity_cache_flags =
                old_identity_entry & cache_mask;
        }
    }

    if (ok && old_identity_cache_flags != cache_flags &&
        !g_platform.flush_caches) {
        g_mmio_mapping_failure_reason =
            "CACHE_FLUSH_BACKEND_UNAVAILABLE";
        ok = false;
    }

    if (ok) {
        if (old_identity_cache_flags != cache_flags) {
            /* Flush existing WB/WT cache lines before changing the
               identity alias memory type. TLB invalidation follows. */
            g_platform.flush_caches();

            identity_pt[identity_index] =
                (old_identity_entry & ~cache_mask) | cache_flags;

            __atomic_thread_fence(__ATOMIC_RELEASE);
            invalidate_shared_kernel_page(physical_address);
        }

        const uint64_t new_entry =
            physical_address | PAGE_PRESENT | PAGE_WRITABLE |
            PAGE_NX | cache_flags;

        mmio_leaf[mmio_leaf_index] = new_entry;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        invalidate_shared_kernel_page(virtual_address);

        g_mmio_records[free_record] = KernelMmioRecord{
            virtual_address,
            physical_address,
            cache_flags,
            old_identity_cache_flags,
            true
        };

        g_mmio_mapping_failure_reason = "PASS";
    }

    unlock_spaces();

    if (g_platform.critical_leave)
        g_platform.critical_leave(saved_flags);

    return ok;
}

bool address_space_unmap_kernel_mmio_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint64_t cache_flags
) {
    g_mmio_mapping_failure_reason = "INVALID_ARGUMENT";

    constexpr uint64_t cache_mask =
        PAGE_PWT | PAGE_PCD | PAGE_PAT_4K;

    if (!valid_kernel_mmio_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        physical_address >= ADDRESS_SPACE_KERNEL_LIMIT ||
        (cache_flags & ~cache_mask) != 0ULL ||
        !g_platform.invalidate_page) {
        return false;
    }

    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;

    lock_spaces();

    bool ok = g_ready;
    volatile uint64_t* root = nullptr;

    if (ok) ok = kernel_mmio_root_table(g_kernel_root, root);

    uint32_t record_index = KERNEL_MMIO_MAX_PAGES;

    for (uint32_t i = 0; ok && i < KERNEL_MMIO_MAX_PAGES; ++i) {
        const KernelMmioRecord& record = g_mmio_records[i];

        if (record.active &&
            record.virtual_address == virtual_address) {
            record_index = i;
            break;
        }
    }

    if (ok && record_index == KERNEL_MMIO_MAX_PAGES) {
        g_mmio_mapping_failure_reason = "MMIO_OWNERSHIP_RECORD_NOT_FOUND";
        ok = false;
    }

    KernelMmioRecord record{};
    if (ok) {
        record = g_mmio_records[record_index];

        if (record.physical_address != physical_address ||
            record.cache_flags != cache_flags) {
            g_mmio_mapping_failure_reason =
                "MMIO_OWNERSHIP_TOKEN_MISMATCH";
            ok = false;
        }
    }

    volatile uint64_t* mmio_leaf = nullptr;
    uint32_t mmio_leaf_index = 0u;

    if (ok && !walk_kernel_mmio_leaf(
            root, virtual_address, false,
            mmio_leaf, mmio_leaf_index)) {
        g_mmio_mapping_failure_reason =
            "MMIO_PAGE_TABLE_WALK_FAILED";
        ok = false;
    }

    constexpr uint64_t verify_mask =
        user_address::PTE_ADDRESS_MASK |
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX |
        PAGE_PWT | PAGE_PCD | PAGE_PAT_4K;

    const uint64_t expected_mmio_entry =
        physical_address | PAGE_PRESENT | PAGE_WRITABLE |
        PAGE_NX | cache_flags;

    if (ok) {
        const uint64_t current_entry = mmio_leaf[mmio_leaf_index];

        if ((current_entry & PAGE_PRESENT) != 0ULL) {
            if ((current_entry & verify_mask) !=
                (expected_mmio_entry & verify_mask)) {
                g_mmio_mapping_failure_reason =
                    "MMIO_LEAF_OWNERSHIP_MISMATCH";
                ok = false;
            } else {
                mmio_leaf[mmio_leaf_index] = 0ULL;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                invalidate_shared_kernel_page(virtual_address);
            }
        } else if (current_entry != 0ULL) {
            g_mmio_mapping_failure_reason =
                "MMIO_LEAF_CONTAINS_NONPRESENT_METADATA";
            ok = false;
        }
        /* A zero leaf is accepted here to permit retry after a previous
           unmap removed the alias but failed to restore identity attributes. */
    }

    volatile uint64_t* identity_pt = nullptr;
    uint32_t identity_index = 0u;
    bool identity_parents_writable = false;

    if (ok && !walk_identity_page_leaf(
            physical_address, identity_pt,
            identity_index, identity_parents_writable)) {
        g_mmio_mapping_failure_reason =
            "IDENTITY_ALIAS_RESTORE_WALK_FAILED";
        ok = false;
    }

    if (ok) {
        const uint64_t current_identity =
            identity_pt[identity_index];

        if ((current_identity & PAGE_PRESENT) == 0ULL ||
            (current_identity & PAGE_USER) != 0ULL ||
            (current_identity & user_address::PTE_ADDRESS_MASK) !=
                physical_address) {
            g_mmio_mapping_failure_reason =
                "IDENTITY_ALIAS_RESTORE_LEAF_MISMATCH";
            ok = false;
        } else {
            const uint64_t current_cache_flags =
                current_identity & cache_mask;

            if (current_cache_flags == record.cache_flags &&
                record.identity_old_cache_flags != record.cache_flags) {
                if (!g_platform.flush_caches) {
                    g_mmio_mapping_failure_reason =
                        "CACHE_FLUSH_BACKEND_UNAVAILABLE";
                    ok = false;
                } else {
                    /* The UC virtual alias has already been removed.
                       Flush before restoring the identity alias type. */
                    g_platform.flush_caches();

                    identity_pt[identity_index] =
                        (current_identity & ~cache_mask) |
                        record.identity_old_cache_flags;

                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    invalidate_shared_kernel_page(physical_address);
                }
            } else if (current_cache_flags !=
                       record.identity_old_cache_flags) {
                g_mmio_mapping_failure_reason =
                    "IDENTITY_CACHE_ATTRIBUTE_CHANGED_EXTERNALLY";
                ok = false;
            }
        }
    }

    if (ok) {
        g_mmio_records[record_index] = KernelMmioRecord{};
        g_mmio_mapping_failure_reason = "PASS";
    }

    unlock_spaces();

    if (g_platform.critical_leave)
        g_platform.critical_leave(saved_flags);

    return ok;
}

bool address_space_kernel_mmio_page_mapped(
    uint64_t root_physical, uint64_t virtual_address,
    uint64_t physical_address, uint64_t cache_flags
) {
    constexpr uint64_t cache_mask =
        PAGE_PWT | PAGE_PCD | PAGE_PAT_4K;

    if (!valid_kernel_mmio_page_address(virtual_address) ||
        !valid_physical_page(physical_address) ||
        (cache_flags & ~cache_mask) != 0ULL) {
        return false;
    }

    lock_spaces();

    volatile uint64_t* root = nullptr;
    bool ok = g_ready &&
        kernel_mmio_root_table(root_physical, root);

    bool owned = false;
    for (uint32_t i = 0; ok && i < KERNEL_MMIO_MAX_PAGES; ++i) {
        const KernelMmioRecord& record = g_mmio_records[i];
        if (record.active &&
            record.virtual_address == virtual_address &&
            record.physical_address == physical_address &&
            record.cache_flags == cache_flags) {
            owned = true;
            break;
        }
    }

    uint64_t entry = 0ULL;
    if (ok && owned) {
        ok = walk_kernel_mmio_leaf_readonly(
            root, virtual_address, entry);
    } else {
        ok = false;
    }

    const uint64_t expected =
        physical_address | PAGE_PRESENT | PAGE_WRITABLE |
        PAGE_NX | cache_flags;

    constexpr uint64_t verify_mask =
        user_address::PTE_ADDRESS_MASK |
        PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NX |
        PAGE_PWT | PAGE_PCD | PAGE_PAT_4K;

    if (ok)
        ok = (entry & verify_mask) == (expected & verify_mask);

    unlock_spaces();
    return ok;
}

const char* address_space_kernel_mmio_mapping_reason() {
    return g_mmio_mapping_failure_reason;
}


} // namespace starlix::mem
