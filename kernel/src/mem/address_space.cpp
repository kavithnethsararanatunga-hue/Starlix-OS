// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "address_space.hpp"
#include "mmio_policy.hpp"
#include "phys.hpp"
#include "managed_page_pool.hpp"
#include "../cpu/msr.hpp"

namespace starlix::mem {
namespace {

constexpr uint64_t ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t DMA32_LIMIT = 0x100000000ULL;
constexpr uint64_t PTE_PRESENT = 1ULL;
constexpr uint64_t PTE_USER = 1ULL << 2;
constexpr uint64_t PTE_HUGE = 1ULL << 7;
constexpr uint64_t CR4_PCIDE = 1ULL << 17;
constexpr uint64_t CR3_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;
static uint64_t g_cr3_control_bits = 0ULL;
static const char* g_address_space_init_reason = "NOT_RUN";

uint64_t irq_save_disable_adapter() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void irq_restore_adapter(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0ULL)
        asm volatile("sti" ::: "memory");
}

void invalidate_page_adapter(uint64_t virtual_address) {
    asm volatile("invlpg (%0)" :: "r"(virtual_address) : "memory");
}

void flush_caches_adapter() {
    asm volatile("mfence; wbinvd; mfence" ::: "memory");
}

bool switch_root_adapter(uint64_t expected_current_root,
                         uint64_t target_root) {
    uint16_t cs = 0;
    uint64_t cr4 = 0ULL;
    uint64_t current_cr3 = 0ULL;
    uint64_t rflags = 0ULL;
    if (target_root == 0ULL || target_root >= DMA32_LIMIT ||
        (target_root & 0xFFFULL) != 0ULL ||
        (target_root & ~CR3_ADDRESS_MASK) != 0ULL ||
        expected_current_root == 0ULL ||
        (expected_current_root & 0xFFFULL) != 0ULL) {
        return false;
    }

    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    asm volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");

    /* No user-originated, interruptible, or PCID-tagged CR3 transitions. */
    if ((cs & 0x3u) != 0u || (rflags & (1ULL << 9)) != 0ULL ||
        (cr4 & CR4_PCIDE) != 0ULL ||
        (current_cr3 & 0xFE7ULL) != 0ULL ||
        (current_cr3 & ~(CR3_ADDRESS_MASK | 0x18ULL)) != 0ULL ||
        (current_cr3 & CR3_ADDRESS_MASK) != expected_current_root) {
        return false;
    }

    const uint64_t target_cr3 = target_root | g_cr3_control_bits;
    /* With PCIDE clear, reloading CR3 flushes non-global translations.
       Kernel global mappings are shared unchanged across every cloned root. */
    asm volatile("mov %0, %%cr3" :: "r"(target_cr3) : "memory");
    uint64_t observed_cr3 = 0ULL;
    asm volatile("mov %%cr3, %0" : "=r"(observed_cr3));
    if ((observed_cr3 & CR3_ADDRESS_MASK) == target_root) return true;

    /* Fail closed: attempt to restore the known prior root before returning. */
    const uint64_t restore_cr3 = expected_current_root | g_cr3_control_bits;
    asm volatile("mov %0, %%cr3" :: "r"(restore_cr3) : "memory");
    return false;
}

uint64_t allocate_page_adapter() {
    /* address_space_init() needs one bootstrap PDPT before the managed lease
       pool is brought online. Later page-table pages are lease tracked. */
    return managed_page_pool_ready()
        ? managed_alloc_phys_page() : alloc_phys_page();
}

bool release_page_adapter(uint64_t physical_address) {
    return managed_release_phys_page(physical_address);
}

volatile uint64_t* map_table_adapter(uint64_t physical_address) {
    if (physical_address == 0ULL ||
        (physical_address & 0xFFFULL) != 0ULL ||
        physical_address >= DMA32_LIMIT ||
        (physical_address & ~ADDRESS_MASK) != 0ULL) {
        return nullptr;
    }
    /* Current kernel memory model is explicitly identity-addressed below 4 GiB. */
    return reinterpret_cast<volatile uint64_t*>(
        static_cast<uintptr_t>(physical_address));
}

void cpuid(uint32_t leaf, uint32_t subleaf,
           uint32_t& eax, uint32_t& ebx,
           uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf));
}

uint32_t physical_address_bits() {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(0x80000000u, 0u, eax, ebx, ecx, edx);
    if (eax < 0x80000008u) return 0u;
    cpuid(0x80000008u, 0u, eax, ebx, ecx, edx);
    return eax & 0xFFu;
}

bool supervisor_identity_mapping(uint64_t root, uint64_t address,
                                 uint32_t physical_bits) {
    if (root == 0ULL || root >= DMA32_LIMIT || address >= DMA32_LIMIT)
        return false;
    const uint64_t physical_mask = ADDRESS_MASK &
        ((1ULL << physical_bits) - 1ULL);
    volatile uint64_t* table = map_table_adapter(root);
    if (!table) return false;
    for (uint32_t level = 4u; level != 0u; --level) {
        const uint32_t shift = 12u + 9u * (level - 1u);
        const uint32_t index = static_cast<uint32_t>((address >> shift) & 0x1FFULL);
        const uint64_t entry = table[index];
        if ((entry & (PTE_PRESENT | PTE_USER)) != PTE_PRESENT) return false;
        const bool huge = (entry & PTE_HUGE) != 0ULL;
        if (huge && (level == 3u || level == 2u)) {
            const uint32_t page_shift = level == 3u ? 30u : 21u;
            const uint64_t base_mask = ADDRESS_MASK & ~((1ULL << page_shift) - 1ULL);
            const uint64_t translated = (entry & base_mask) |
                (address & ((1ULL << page_shift) - 1ULL));
            return translated == address &&
                ((entry & base_mask) & ~physical_mask) == 0ULL;
        }
        if (level == 1u) {
            const uint64_t translated = (entry & ADDRESS_MASK) |
                (address & 0xFFFULL);
            return translated == address &&
                ((entry & ADDRESS_MASK) & ~physical_mask) == 0ULL;
        }
        if (huge) return false;
        const uint64_t child = entry & ADDRESS_MASK;
        if (child == 0ULL || child >= DMA32_LIMIT ||
            (child & ~physical_mask) != 0ULL) return false;
        table = map_table_adapter(child);
        if (!table) return false;
    }
    return false;
}

} // namespace

bool address_space_init() {
    g_address_space_init_reason = "PHYSICAL_ALLOCATOR_UNAVAILABLE";
    if (!phys_allocator_ready()) return false;
    AddressSpacePlatform platform{};
    asm volatile("mov %%cr0, %0" : "=r"(platform.cr0));
    asm volatile("mov %%cr3, %0" : "=r"(platform.cr3));
    asm volatile("mov %%cr4, %0" : "=r"(platform.cr4));
    platform.efer = starlix::cpu::rdmsr(starlix::cpu::MSR_EFER);
    platform.physical_address_bits = physical_address_bits();
    const uint64_t root = platform.cr3 & CR3_ADDRESS_MASK;
    uint64_t current_rsp = 0ULL;
    asm volatile("mov %%rsp,%0" : "=r"(current_rsp));
    g_address_space_init_reason = "KERNEL_CODE_OR_STACK_IDENTITY_MAPPING_INVALID";
    if (platform.physical_address_bits < 32u ||
        platform.physical_address_bits > 52u ||
        !supervisor_identity_mapping(root,
            reinterpret_cast<uint64_t>(&address_space_init),
            platform.physical_address_bits) ||
        !supervisor_identity_mapping(root, current_rsp,
            platform.physical_address_bits)) return false;
    platform.allocate_page = allocate_page_adapter;
    platform.release_page = release_page_adapter;
    platform.map_table = map_table_adapter;
    platform.critical_enter = irq_save_disable_adapter;
    platform.critical_leave = irq_restore_adapter;
    platform.switch_root = switch_root_adapter;
    platform.invalidate_page = invalidate_page_adapter;
    platform.flush_caches = flush_caches_adapter;
    /* This source tree currently starts only the bootstrap processor: there
       is no AP startup, Local-APIC IPI, or IOAPIC routing backend. Do not infer
       SMP support from CPUID's APIC feature bit. The generic address-space
       manager rejects online_cpu_count != 1 until a real shootdown backend
       and per-CPU active-root tracking are integrated. */
    platform.online_cpu_count = 1u;
    g_cr3_control_bits = platform.cr3 & 0x18ULL;
    const bool initialized = address_space_manager_init(platform);
    g_address_space_init_reason = initialized
        ? "PASS" : address_space_failure_reason();
    if (!initialized) g_cr3_control_bits = 0ULL;
    return initialized;
}


bool address_space_query_uc_pat_flags(
    uint64_t& out_cache_flags, uint8_t& out_pat_index
) {
    out_cache_flags = mmio_policy::INVALID_CACHE_FLAGS;
    out_pat_index = 0xFFu;

    uint32_t max_basic = 0u;
    uint32_t ebx = 0u;
    uint32_t ecx = 0u;
    uint32_t edx = 0u;

    cpuid(0u, 0u, max_basic, ebx, ecx, edx);
    if (max_basic < 1u) return false;

    uint32_t eax = 0u;
    cpuid(1u, 0u, eax, ebx, ecx, edx);

    /* Do not read IA32_PAT unless CPUID confirms MSR and PAT support. */
    if ((edx & (mmio_policy::CPUID_FEATURE_MSR |
                mmio_policy::CPUID_FEATURE_PAT)) !=
               (mmio_policy::CPUID_FEATURE_MSR |
                mmio_policy::CPUID_FEATURE_PAT)) {
        return false;
    }

    const uint64_t pat_value =
        starlix::cpu::rdmsr(starlix::cpu::MSR_IA32_PAT);

    return mmio_policy::select_uc_pat_flags(
        max_basic, edx, pat_value,
        out_pat_index, out_cache_flags);
}

const char* address_space_init_failure_reason() {
    return g_address_space_init_reason;
}

} // namespace starlix::mem
