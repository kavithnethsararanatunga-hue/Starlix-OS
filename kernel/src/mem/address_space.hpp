// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "user_address.hpp"

namespace starlix::mem {

constexpr uint32_t ADDRESS_SPACE_MAX = 32u;
constexpr uint32_t ADDRESS_SPACE_MAGIC = 0x53504143u; // "SPAC"
constexpr uint64_t ADDRESS_SPACE_KERNEL_LIMIT = 0x100000000ULL;
constexpr uint32_t KERNEL_MMIO_PML4_INDEX = 508u;
constexpr uint64_t KERNEL_MMIO_REGION_BASE = 0xFFFFFE0000000000ULL;
constexpr uint64_t KERNEL_MMIO_REGION_END  = 0xFFFFFE8000000000ULL;
constexpr uint32_t KERNEL_MMIO_MAX_PAGES = 4096u;

constexpr uint32_t KERNEL_MODULE_PML4_INDEX = 509u;
constexpr uint64_t KERNEL_MODULE_REGION_BASE = 0xFFFFFE8000000000ULL;
constexpr uint64_t KERNEL_MODULE_REGION_END  = 0xFFFFFF0000000000ULL;
constexpr uint32_t KERNEL_MODULE_MAP_WRITE = 1u << 0;
constexpr uint32_t KERNEL_MODULE_MAP_EXECUTE = 1u << 1;
constexpr uint32_t KERNEL_MODULE_MAP_FLAGS_ALL =
    KERNEL_MODULE_MAP_WRITE | KERNEL_MODULE_MAP_EXECUTE;
constexpr uint32_t KERNEL_MAPPING_READ = 1u << 0;
constexpr uint32_t KERNEL_MAPPING_WRITE = 1u << 1;
constexpr uint32_t KERNEL_MAPPING_EXECUTE = 1u << 2;
constexpr uint32_t KERNEL_MAPPING_USER = 1u << 3;
constexpr uint32_t KERNEL_STACK_PML4_INDEX = 510u;
constexpr uint64_t KERNEL_STACK_REGION_BASE = 0xFFFFFF0000000000ULL;
constexpr uint64_t KERNEL_STACK_REGION_END  = 0xFFFFFF8000000000ULL;
constexpr uint64_t KERNEL_STACK_PAGE_SIZE = 4096ULL;
constexpr uint32_t KERNEL_STACK_MAX_MAP_PAGES = 16u;

struct AddressSpace {
    uint64_t cr3;
    uint32_t mapped_pages;
    uint32_t magic;
};

struct KernelMappingInfo {
    uint64_t physical_address;
    uint32_t permissions;
    uint32_t page_size;
};

using AddressSpaceAllocatePageFn = uint64_t (*)();
using AddressSpaceReleasePageFn = bool (*)(uint64_t physical_address);
using AddressSpaceMapTableFn = volatile uint64_t* (*)(uint64_t physical_address);
using AddressSpaceCriticalEnterFn = uint64_t (*)();
using AddressSpaceCriticalLeaveFn = void (*)(uint64_t saved_flags);
using AddressSpaceSwitchRootFn = bool (*)(uint64_t expected_current_root,
                                           uint64_t target_root);
using AddressSpaceInvalidatePageFn = void (*)(uint64_t virtual_address);
using AddressSpaceFlushCachesFn = void (*)();

/* Snapshot + callbacks make the page-table core independently host-testable.
   The kernel adapter supplies live CR0/CR3/CR4/EFER values and identity mapping. */
struct AddressSpacePlatform {
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t efer;
    uint32_t physical_address_bits;
    AddressSpaceAllocatePageFn allocate_page;
    AddressSpaceReleasePageFn release_page; // Optional reclaim hook for rollback/pruning.
    AddressSpaceMapTableFn map_table;
    AddressSpaceCriticalEnterFn critical_enter;
    AddressSpaceCriticalLeaveFn critical_leave;
    AddressSpaceSwitchRootFn switch_root;
    AddressSpaceInvalidatePageFn invalidate_page;
    /* Required by MMIO cache-type transitions; the live x86 adapter uses
       WBINVD while the BSP is in the existing interrupt-disabled section. */
    AddressSpaceFlushCachesFn flush_caches;
    /* This address-space implementation is intentionally single-CPU only.
       The adapter must report the real number of online processors. Until a
       synchronous SMP shootdown + per-CPU active-root backend exists, values
       other than 1 are rejected during initialization. */
    uint32_t online_cpu_count;
};

bool address_space_manager_init(const AddressSpacePlatform& platform);
const char* address_space_failure_reason();
/* Read-only CPU inspection; never loads CR3 or enables Ring 3. */
bool address_space_init();
/* Read-only CPUID + IA32_PAT probe. Does not modify PAT MSR. */
bool address_space_query_uc_pat_flags(
    uint64_t& out_cache_flags, uint8_t& out_pat_index
);
const char* address_space_init_failure_reason();
bool address_space_manager_ready();
/* Reports the CPU topology accepted by the live manager; 0 means unavailable. */
uint32_t address_space_online_cpu_count();
/* Monotonic count of shared-kernel page invalidations performed by this manager.
   On this build it records local INVLPG operations only, not remote shootdowns. */
uint64_t address_space_tlb_invalidation_generation();
uint64_t address_space_kernel_root();

/* Clone the current kernel PML4. Existing mappings are shared but supervisor-
   only in the clone. The reserved user PML4 slot gets private page tables. */
bool address_space_create(AddressSpace& out_space);
/* Destroy only an inactive clone with no remaining user mappings. */
bool address_space_destroy(AddressSpace& space);

/* Activate an already-created address space from Ring 0. The adapter disables
   interrupts around the CR3 write, checks the current CR3/CPL/PCIDE state, and
   restores the caller's IF state. Activation is single-CPU only in this phase. */
bool address_space_activate(const AddressSpace& space);
bool address_space_activate_kernel();
/* Resolve/activate a root already registered by address_space_create(). */
bool address_space_root_registered(uint64_t root_physical);
bool address_space_activate_root(uint64_t root_physical);
uint64_t address_space_active_root();

/* PML4 slot 510 is reserved at manager initialization for a shared,
   supervisor-only, NX kernel-stack arena. Its page-table subtree is shared by
   every cloned process root. Stack mappings are new virtual addresses; the
   physical allocator remains identity-addressed below 4 GiB. */
bool address_space_map_kernel_stack_range(
    uint64_t virtual_base, uint64_t physical_base, uint32_t page_count
);
/* Remove an exact shared supervisor/NX stack run from the kernel stack arena.
   The VA is never recycled; only its physical pages are made reusable. */
bool address_space_unmap_kernel_stack_range(
    uint64_t virtual_base, uint64_t physical_base, uint32_t page_count
);
bool address_space_kernel_stack_range_mapped(
    uint64_t root_physical, uint64_t virtual_base,
    uint64_t physical_base, uint32_t page_count
);
const char* address_space_kernel_stack_mapping_reason();
bool address_space_kernel_stack_guard_unmapped(
    uint64_t root_physical, uint64_t guard_page_address
);

/* Generic supervisor-only external-module arena (PML4 slot 509). Every
   module leaf is either RO+X, RO+NX, or RW+NX; RW+X is never accepted.
   The direct identity alias is inspected before mapping so a writable alias
   cannot coexist with an executable module alias, or vice versa. */
bool address_space_query_kernel_mapping(
    uint64_t root_physical, uint64_t virtual_address,
    KernelMappingInfo& out_info
);
bool address_space_map_kernel_module_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint32_t mapping_flags
);
bool address_space_unmap_kernel_module_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint32_t mapping_flags
);
bool address_space_kernel_module_page_mapped(
    uint64_t root_physical, uint64_t virtual_address,
    uint64_t physical_address, uint32_t mapping_flags
);
bool address_space_kernel_module_guard_unmapped(
    uint64_t root_physical, uint64_t guard_page_address
);
const char* address_space_kernel_module_mapping_reason();

/* Generic supervisor-only, writable, NX MMIO page mappings in PML4 slot
   508. cache_flags are the PWT/PCD/PAT bits for a 4-KiB PTE. The mapping
   API requires the low identity alias to exist so its cache type can be
   changed consistently and restored during unmap. Current implementation
   supports physical MMIO below 4 GiB and one online CPU only. */
bool address_space_map_kernel_mmio_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint64_t cache_flags
);
bool address_space_unmap_kernel_mmio_page(
    uint64_t virtual_address, uint64_t physical_address,
    uint64_t cache_flags
);
bool address_space_kernel_mmio_page_mapped(
    uint64_t root_physical, uint64_t virtual_address,
    uint64_t physical_address, uint64_t cache_flags
);
const char* address_space_kernel_mmio_mapping_reason();

/* Change only the low physical identity alias of a managed 4 KiB page.
   The identity alias is always left NX; executable code is mapped only in
   the separate module arena. Huge 1 GiB/2 MiB identity leaves are demoted
   with their original cache/PAT attributes preserved. Expected permissions
   use KERNEL_MAPPING_WRITE / KERNEL_MAPPING_EXECUTE; USER is never allowed.
   This updates the shared kernel identity subtree and uses the centralized
   kernel-page invalidation path. The current adapter advertises one online CPU;
   manager initialization rejects SMP until a synchronous all-CPU TLB shootdown
   and per-CPU active-root implementation are available. It is not a scan of
   arbitrary kernel aliases. */
bool address_space_set_kernel_identity_page_permissions(
    uint64_t physical_address, uint32_t expected_permissions,
    uint32_t target_permissions
);
const char* address_space_kernel_identity_permission_reason();

/* Map/unmap one 4 KiB user page. The target space must be inactive; active
   mappings are immutable until a later TLB shootdown/invalidation phase. */
bool address_space_map_user_page(
    AddressSpace& space,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
);
bool address_space_unmap_user_page(
    AddressSpace& space,
    uint64_t virtual_address
);
bool address_space_translate_user(
    const AddressSpace& space,
    uint64_t virtual_address,
    uint64_t& out_physical_address,
    uint32_t& out_permissions
);

} // namespace starlix::mem
