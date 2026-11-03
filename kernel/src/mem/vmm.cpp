// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "vmm.hpp"
#include "address_space.hpp"
#include "../cpu/msr.hpp"
#include "../drivers/serial.hpp"

namespace starlix::mem {

static uint64_t user_heap_current = 0x00007FFFF0000000ULL;

// Diagnostic output has independent character and polling budgets.
static uint32_t diag_chars_left = 1536u;
static uint32_t diag_poll_budget = 2000000u;
static const char* nxe_failure = "NOT_RUN";

namespace {
constexpr uint64_t PTE_ADDR = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t FOUR_GIB = 0x100000000ULL;
constexpr uint64_t PTE_PRESENT = 1ULL;
constexpr uint64_t PTE_HUGE = 1ULL << 7;
constexpr uint64_t PTE_NX = 1ULL << 63;
constexpr uint64_t CR0_PG = 1ULL << 31;
constexpr uint64_t CR4_PAE = 1ULL << 5;
constexpr uint64_t CR4_LA57 = 1ULL << 12;
constexpr uint64_t CR4_PCIDE = 1ULL << 17;

static bool inspect_table(uint64_t physical, uint32_t level,
                          uint32_t physical_bits) {
    if (physical == 0 || (physical & 0xFFFULL) != 0 || physical >= FOUR_GIB)
        return false;
    const volatile uint64_t* table =
        reinterpret_cast<const volatile uint64_t*>(static_cast<uintptr_t>(physical));
    const uint64_t address_mask = PTE_ADDR & ((1ULL << physical_bits) - 1ULL);
    for (uint32_t i = 0; i < 512; ++i) {
        const uint64_t entry = table[i];
        if ((entry & PTE_PRESENT) == 0) continue;
        /* NX is reserved until NXE is set; bits 52..62 are always reserved
           for this non-encrypted, non-keyed paging configuration. */
        if ((entry & (PTE_NX | 0x7FF0000000000000ULL)) != 0 ||
            ((entry & PTE_ADDR) & ~address_mask) != 0) return false;
        const bool huge = (entry & PTE_HUGE) != 0;
        if (huge && (level == 4 || level == 1)) return false;
        if (huge) {
            const uint32_t shift = level == 3 ? 30u : 21u;
            const uint64_t reserved_low = ((1ULL << shift) - 1ULL) & ~0x1FFFULL;
            if ((entry & reserved_low) != 0) return false;
            continue;
        }
        if (level > 1 && !inspect_table(entry & PTE_ADDR, level - 1, physical_bits))
            return false;
    }
    return true;
}
}

bool vmm_enable_nxe() {
    uint32_t eax=0, ebx=0, ecx=0, edx=0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x80000000u), "c"(0u));
    if (eax < 0x80000001u) { nxe_failure = "CPUID_EXTENDED_LEAF_MISSING"; return false; }
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x80000001u), "c"(0u));
    if ((edx & (1u << 20)) == 0) { nxe_failure = "CPU_NX_UNSUPPORTED"; return false; }
    uint64_t cr0=0, cr3=0, cr4=0;
    asm volatile("mov %%cr0,%0" : "=r"(cr0));
    asm volatile("mov %%cr3,%0" : "=r"(cr3));
    asm volatile("mov %%cr4,%0" : "=r"(cr4));
    if ((cr0 & CR0_PG) == 0 || (cr4 & CR4_PAE) == 0 ||
        (cr4 & (CR4_LA57|CR4_PCIDE)) != 0) {
        nxe_failure = "UNSUPPORTED_ACTIVE_PAGING_MODE"; return false;
    }
    uint32_t max=0;
    asm volatile("cpuid" : "=a"(max), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x80000000u), "c"(0u));
    if (max < 0x80000008u) { nxe_failure = "PHYSICAL_WIDTH_UNAVAILABLE"; return false; }
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x80000008u), "c"(0u));
    const uint32_t bits = eax & 0xFFu;
    const uint64_t root = cr3 & PTE_ADDR;
    if (bits < 32 || bits > 52 || root == 0 || (root & 0xFFFULL) != 0 ||
        root >= FOUR_GIB || (root & ~(PTE_ADDR & ((1ULL << bits)-1ULL))) != 0) {
        nxe_failure = "CR3_ROOT_INVALID_OR_INACCESSIBLE"; return false;
    }
    const uint64_t efer = starlix::cpu::rdmsr(starlix::cpu::MSR_EFER);
    if ((efer & (1ULL << 10)) == 0) { nxe_failure = "LONG_MODE_INACTIVE"; return false; }
    if ((efer & starlix::cpu::EFER_NXE) != 0) { nxe_failure = "ALREADY_ENABLED"; return true; }
    if (!inspect_table(root, 4, bits)) {
        nxe_failure = "INHERITED_PAGE_TABLE_RESERVED_BITS_OR_UNMAPPED_TABLE"; return false;
    }
    starlix::cpu::wrmsr(starlix::cpu::MSR_EFER, efer | starlix::cpu::EFER_NXE);
    const uint64_t observed = starlix::cpu::rdmsr(starlix::cpu::MSR_EFER);
    if ((observed & starlix::cpu::EFER_NXE) == 0 ||
        (observed & ~starlix::cpu::EFER_NXE) != (efer & ~starlix::cpu::EFER_NXE)) {
        nxe_failure = "EFER_NXE_READBACK_OR_PRESERVATION_FAILED"; return false;
    }
    nxe_failure = "PASS";
    return true;
}

const char* vmm_nxe_failure_reason() { return nxe_failure; }

static bool diag_putc(char value) {
    if (diag_chars_left == 0 || diag_poll_budget == 0)
        return false;

    while (diag_poll_budget != 0) {
        --diag_poll_budget;

        if ((starlix::cpu::inb(0x3FDu) & 0x20u) != 0) {
            starlix::cpu::outb(0x3F8u,
                static_cast<uint8_t>(value));
            --diag_chars_left;
            return true;
        }

        asm volatile("pause" ::: "memory");
    }

    return false;
}

static void diag_write(const char* text) {
    if (!text) return;

    while (*text) {
        if (!diag_putc(*text++))
            return;
    }
}

static void diag_hex64(const char* label, uint64_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";

    diag_write(label);
    diag_write("=0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        const uint8_t digit =
            static_cast<uint8_t>((value >> shift) & 0x0Fu);

        if (!diag_putc(digits[digit]))
            return;
    }

    diag_write("\r\n");
}

static void diag_flag(const char* label, bool enabled) {
    diag_write(label);
    diag_write(enabled ? "=1\r\n" : "=0\r\n");
}

static void diag_u32(const char* label, uint32_t value) {
    char buffer[11]{};
    uint32_t count = 0;

    do {
        buffer[count++] =
            static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(buffer));

    diag_write(label);
    diag_write("=");

    while (count != 0) {
        if (!diag_putc(buffer[--count]))
            return;
    }

    diag_write("\r\n");
}

static void read_cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t& eax,
    uint32_t& ebx,
    uint32_t& ecx,
    uint32_t& edx
) {
    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
    );
}

/*
 * Read-only inspection of the inherited UEFI paging environment.
 * This function deliberately does not:
 * - write CR0, CR3 or CR4;
 * - modify EFER;
 * - dereference CR3 or walk page tables;
 * - allocate or map memory.
 */
static void paging_diagnostics() {
    uint64_t cr0 = 0;
    uint64_t cr3 = 0;
    uint64_t cr4 = 0;

    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    const uint64_t efer =
        starlix::cpu::rdmsr(starlix::cpu::MSR_EFER);

    diag_write("\r\n========== STARLIX VMM DIAGNOSTICS ==========\r\n");

    diag_hex64("CR0", cr0);
    diag_hex64("CR3_RAW", cr3);
    diag_hex64("CR4", cr4);
    diag_hex64("EFER", efer);

    diag_flag("CR0.PE", (cr0 & (1ULL << 0)) != 0);
    diag_flag("CR0.WP", (cr0 & (1ULL << 16)) != 0);
    diag_flag("CR0.PG", (cr0 & (1ULL << 31)) != 0);

    diag_flag("CR4.PAE", (cr4 & (1ULL << 5)) != 0);
    diag_flag("CR4.PGE", (cr4 & (1ULL << 7)) != 0);
    diag_flag("CR4.LA57", (cr4 & (1ULL << 12)) != 0);
    diag_flag("CR4.PCIDE", (cr4 & (1ULL << 17)) != 0);
    diag_flag("CR4.SMEP", (cr4 & (1ULL << 20)) != 0);
    diag_flag("CR4.SMAP", (cr4 & (1ULL << 21)) != 0);

    diag_flag("EFER.LME", (efer & (1ULL << 8)) != 0);
    diag_flag("EFER.LMA", (efer & (1ULL << 10)) != 0);
    diag_flag("EFER.NXE", (efer & (1ULL << 11)) != 0);
    diag_flag("EFER.SCE", (efer & (1ULL << 0)) != 0);

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    read_cpuid(0, 0, eax, ebx, ecx, edx);
    const uint32_t max_basic = eax;

    read_cpuid(0x80000000u, 0, eax, ebx, ecx, edx);
    const uint32_t max_extended = eax;

    if (max_basic >= 1u) {
        read_cpuid(1, 0, eax, ebx, ecx, edx);
        diag_flag("CPUID.PAE", (edx & (1u << 6)) != 0);
    }

    if (max_extended >= 0x80000001u) {
        read_cpuid(0x80000001u, 0, eax, ebx, ecx, edx);
        diag_flag("CPUID.SYSCALL", (edx & (1u << 11)) != 0);
        diag_flag("CPUID.NX", (edx & (1u << 20)) != 0);
        diag_flag("CPUID.LM", (edx & (1u << 29)) != 0);
    }

    if (max_extended >= 0x80000008u) {
        read_cpuid(0x80000008u, 0, eax, ebx, ecx, edx);
        diag_u32("CPU.PHYSICAL_ADDRESS_BITS", eax & 0xFFu);
        diag_u32("CPU.LINEAR_ADDRESS_BITS", (eax >> 8) & 0xFFu);
    }

    if (max_basic >= 7u) {
        read_cpuid(7, 0, eax, ebx, ecx, edx);
        diag_flag("CPUID.SMEP", (ebx & (1u << 7)) != 0);
        diag_flag("CPUID.INVPCID", (ebx & (1u << 10)) != 0);
        diag_flag("CPUID.SMAP", (ebx & (1u << 20)) != 0);
        diag_flag("CPUID.LA57", (ecx & (1u << 16)) != 0);
    }

    diag_write(
        "NOTE=CPU_REGISTERS_ONLY; INITIAL_PROBE_DOES_NOT_MODIFY_PAGING\r\n");

    diag_write("=============================================\r\n");
}

void vmm_init() {
    paging_diagnostics();
    const bool address_space_ready =
        ((starlix::cpu::rdmsr(starlix::cpu::MSR_EFER) &
          starlix::cpu::EFER_NXE) != 0ULL) && address_space_init();
    diag_flag("VMM.USER_ADDRESS_SPACE_READY", address_space_ready);
    diag_write(address_space_ready ? "[VMM] ADDRESS_SPACE_MANAGER=PASS\r\n"
                                   : "[VMM] ADDRESS_SPACE_MANAGER=FAIL\r\n");
    if (!address_space_ready) {
        diag_write("[VMM] ADDRESS_SPACE_REASON=");
        diag_write((starlix::cpu::rdmsr(starlix::cpu::MSR_EFER) &
                    starlix::cpu::EFER_NXE) == 0ULL
            ? vmm_nxe_failure_reason()
            : address_space_init_failure_reason());
        diag_write("\r\n");
    }
    if (address_space_ready) {
        diag_write(
            "VMM.MODE=4_LEVEL; KERNEL_ROOT_CLONING_ENABLED; USER_SLOT=1\r\n");
    } else {
        diag_write(
            "VMM.MODE=SAFE_DISABLED; INHERITED_PAGING_LAYOUT_NOT_ACCEPTED\r\n");
    }
    if (address_space_ready) {
        diag_write(
            "VMM.NOTE=CR3_SWITCH_API_READY; NO_SCHEDULER; NO_RING3_ENTRY; NO_LIVE_KERNEL_PTE_MODIFICATION\r\n");
    } else {
        diag_write(
            "VMM.NOTE=CR3_SWITCH_DISABLED; NO_SCHEDULER; NO_RING3_ENTRY\r\n");
    }
}

uint64_t StNtAllocateVirtualMemory(uint64_t size, uint32_t protect_flags) {
    (void)protect_flags;

    uint64_t allocated_base = user_heap_current;

    uint64_t page_aligned_size =
        (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    user_heap_current += page_aligned_size;

    return allocated_base;
}

} // namespace starlix::mem
