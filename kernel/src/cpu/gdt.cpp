// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "gdt.hpp"
#include "../mem/user_address.hpp"

namespace starlix::cpu {

#pragma pack(push, 1)

struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
};

struct TssEntry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

struct TssDescriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper32;
    uint32_t reserved;
};

struct GdtPointer {
    uint16_t limit;
    uint64_t base;
};

#pragma pack(pop)

static uint64_t gdt_table[7];
static TssEntry starlix_tss;
static GdtPointer gdt_ptr;

// Dedicated emergency exception stacks.
constexpr size_t EXCEPTION_IST_STACK_SIZE = 16 * 1024;

alignas(4096) static uint8_t nmi_ist_stack[EXCEPTION_IST_STACK_SIZE];
alignas(4096) static uint8_t double_fault_ist_stack[EXCEPTION_IST_STACK_SIZE];
alignas(4096) static uint8_t machine_check_ist_stack[EXCEPTION_IST_STACK_SIZE];
alignas(4096) static uint8_t exception_ist_stack[EXCEPTION_IST_STACK_SIZE];

static_assert(sizeof(TssEntry) == 104,
              "x86-64 TSS layout must be exactly 104 bytes");

extern "C" void asm_load_gdt(uint64_t pointer);
extern "C" void asm_load_tss(uint16_t selector);

// Custom memset for freestanding mode
static void mem_set(void* dest, uint8_t val, size_t count) {
    uint8_t* d = reinterpret_cast<uint8_t*>(dest);
    for (size_t i = 0; i < count; i++) {
        d[i] = val;
    }
}

static void create_gdt_entry(int index, uint8_t access, uint8_t granularity) {
    GdtEntry* entry = reinterpret_cast<GdtEntry*>(&gdt_table[index]);
    entry->limit_low = 0xFFFF;
    entry->base_low = 0;
    entry->base_middle = 0;
    entry->access = access;
    entry->granularity = granularity | 0x0F;
    entry->base_high = 0;
}

void gdt_init() {
    gdt_table[0] = 0;
    create_gdt_entry(1, 0x9A, 0x20); // KCode
    create_gdt_entry(2, 0x92, 0x00); // KData
    create_gdt_entry(3, 0xF2, 0x00); // UData
    create_gdt_entry(4, 0xFA, 0x20); // UCode

    mem_set(&starlix_tss, 0, sizeof(TssEntry));

    // IST1: NMI, IST2: Double Fault, IST3: Machine Check.
    starlix_tss.ist1 =
        reinterpret_cast<uint64_t>(&nmi_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    starlix_tss.ist2 =
        reinterpret_cast<uint64_t>(&double_fault_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    starlix_tss.ist3 =
        reinterpret_cast<uint64_t>(&machine_check_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    starlix_tss.ist4 =
        reinterpret_cast<uint64_t>(&exception_ist_stack[EXCEPTION_IST_STACK_SIZE]);

    // No I/O permission bitmap: offset is beyond the TSS descriptor limit.
    starlix_tss.iopb_offset = static_cast<uint16_t>(sizeof(TssEntry));

    uint64_t tss_base = reinterpret_cast<uint64_t>(&starlix_tss);
    uint32_t tss_limit = sizeof(TssEntry) - 1;

    TssDescriptor* tss_desc = reinterpret_cast<TssDescriptor*>(&gdt_table[5]);
    tss_desc->limit_low = tss_limit & 0xFFFF;
    tss_desc->base_low = tss_base & 0xFFFF;
    tss_desc->base_middle = (tss_base >> 16) & 0xFF;
    tss_desc->access = 0x89;
    tss_desc->granularity = 0x00;
    tss_desc->base_high = (tss_base >> 24) & 0xFF;
    tss_desc->base_upper32 = (tss_base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved = 0;

    gdt_ptr.limit = sizeof(gdt_table) - 1;
    gdt_ptr.base = reinterpret_cast<uint64_t>(&gdt_table);

    asm_load_gdt(reinterpret_cast<uint64_t>(&gdt_ptr));
    asm_load_tss(TSS_SEGMENT);
}

void tss_set_kernel_stack(uint64_t stack_ptr) {
    (void)tss_set_kernel_stack_checked(stack_ptr);
}

bool tss_set_kernel_stack_checked(uint64_t stack_ptr) {
    using namespace starlix::mem::user_address;
    if (stack_ptr == 0ULL || (stack_ptr & 0xFULL) != 0ULL ||
        !is_canonical_48(stack_ptr) ||
        is_user_address(stack_ptr - 1ULL)) {
        return false;
    }
    starlix_tss.rsp0 = stack_ptr;
    return true;
}

uint64_t tss_get_kernel_stack() {
    return starlix_tss.rsp0;
}

bool tss_exception_stacks_ready() {
    const uint64_t nmi_bottom =
        reinterpret_cast<uint64_t>(&nmi_ist_stack[0]);
    const uint64_t df_bottom =
        reinterpret_cast<uint64_t>(&double_fault_ist_stack[0]);
    const uint64_t mc_bottom =
        reinterpret_cast<uint64_t>(&machine_check_ist_stack[0]);
    const uint64_t ex_bottom =
        reinterpret_cast<uint64_t>(&exception_ist_stack[0]);

    const uint64_t nmi_top =
        reinterpret_cast<uint64_t>(&nmi_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    const uint64_t df_top =
        reinterpret_cast<uint64_t>(&double_fault_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    const uint64_t mc_top =
        reinterpret_cast<uint64_t>(&machine_check_ist_stack[EXCEPTION_IST_STACK_SIZE]);
    const uint64_t ex_top =
        reinterpret_cast<uint64_t>(&exception_ist_stack[EXCEPTION_IST_STACK_SIZE]);

    const bool aligned =
        ((nmi_bottom | df_bottom | mc_bottom | ex_bottom) & 0xFFFULL) == 0 &&
        ((nmi_top | df_top | mc_top | ex_top) & 0xFULL) == 0;

    const bool disjoint =
        (nmi_top <= df_bottom || df_top <= nmi_bottom) &&
        (nmi_top <= mc_bottom || mc_top <= nmi_bottom) &&
        (nmi_top <= ex_bottom || ex_top <= nmi_bottom) &&
        (df_top <= mc_bottom || mc_top <= df_bottom) &&
        (df_top <= ex_bottom || ex_top <= df_bottom) &&
        (mc_top <= ex_bottom || ex_top <= mc_bottom);

    return starlix_tss.ist1 == nmi_top &&
           starlix_tss.ist2 == df_top &&
           starlix_tss.ist3 == mc_top &&
           starlix_tss.ist4 == ex_top &&
           (nmi_top - nmi_bottom) == EXCEPTION_IST_STACK_SIZE &&
           (df_top - df_bottom) == EXCEPTION_IST_STACK_SIZE &&
           (mc_top - mc_bottom) == EXCEPTION_IST_STACK_SIZE &&
           (ex_top - ex_bottom) == EXCEPTION_IST_STACK_SIZE &&
           aligned && disjoint &&
           starlix_tss.iopb_offset == sizeof(TssEntry);
}

} // namespace starlix::cpu


namespace starlix::cpu {

bool tss_exception_stack_contains(
    uint8_t ist_index, uint64_t address, size_t length
) {
    if (length == 0 || length > EXCEPTION_IST_STACK_SIZE)
        return false;

    uint64_t bottom = 0;

    switch (ist_index) {
        case 1:
            bottom = reinterpret_cast<uint64_t>(&nmi_ist_stack[0]);
            break;
        case 2:
            bottom = reinterpret_cast<uint64_t>(&double_fault_ist_stack[0]);
            break;
        case 3:
            bottom = reinterpret_cast<uint64_t>(&machine_check_ist_stack[0]);
            break;
        case 4:
            bottom = reinterpret_cast<uint64_t>(&exception_ist_stack[0]);
            break;
        default:
            return false;
    }

    if ((address & 0xFULL) != 0 || address < bottom)
        return false;

    return (address - bottom) <=
           (EXCEPTION_IST_STACK_SIZE - length);
}

} // namespace starlix::cpu
