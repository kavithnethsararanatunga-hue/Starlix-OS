// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "idt.hpp"
#include "io.hpp"
#include "exception.hpp"
#include "interrupt_frame.hpp"
#include "../process/context_switch.hpp"
#include "../process/user_mode_smoke_test.hpp"

namespace starlix::cpu {

#pragma pack(push, 1)
struct IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
};

struct IdtPointer {
    uint16_t limit;
    uint64_t base;
};
#pragma pack(pop)

static IdtEntry idt_entries[256];
static IdtPointer idt_ptr;
static IrqHandler irq_handlers[16]{};
static volatile uint64_t irq_dispatch_counts[16]{};
static uint64_t pit_timer_ticks = 0u;
static uint32_t pit_timer_divisor = 0u;
static uint32_t pit_timer_frequency = 0u;
static uint32_t pit_timer_ready = 0u;
static void pit_timer_irq_handler(uint8_t irq);

extern "C" void asm_load_idt(uint64_t pointer);
extern "C" void isr_default_handler();
extern "C" void isr_exception_0();
extern "C" void isr_exception_1();
extern "C" void isr_exception_2();
extern "C" void isr_exception_3();
extern "C" void isr_exception_4();
extern "C" void isr_exception_5();
extern "C" void isr_exception_6();
extern "C" void isr_exception_7();
extern "C" void isr_exception_8();
extern "C" void isr_exception_9();
extern "C" void isr_exception_10();
extern "C" void isr_exception_11();
extern "C" void isr_exception_12();
extern "C" void isr_exception_13();
extern "C" void isr_exception_14();
extern "C" void isr_exception_15();
extern "C" void isr_exception_16();
extern "C" void isr_exception_17();
extern "C" void isr_exception_18();
extern "C" void isr_exception_19();
extern "C" void isr_exception_20();
extern "C" void isr_exception_21();
extern "C" void isr_exception_22();
extern "C" void isr_exception_23();
extern "C" void isr_exception_24();
extern "C" void isr_exception_25();
extern "C" void isr_exception_26();
extern "C" void isr_exception_27();
extern "C" void isr_exception_28();
extern "C" void isr_exception_29();
extern "C" void isr_exception_30();
extern "C" void isr_exception_31();
extern "C" void isr_irq0();  extern "C" void isr_irq1();
extern "C" void isr_irq2();  extern "C" void isr_irq3();
extern "C" void isr_irq4();  extern "C" void isr_irq5();
extern "C" void isr_irq6();  extern "C" void isr_irq7();
extern "C" void isr_irq8();  extern "C" void isr_irq9();
extern "C" void isr_irq10(); extern "C" void isr_irq11();
extern "C" void isr_irq12(); extern "C" void isr_irq13();
extern "C" void isr_irq14(); extern "C" void isr_irq15();
extern "C" void isr_user_service();
#if STARLIX_ENABLE_RING3_SMOKE_TEST
extern "C" void isr_user_probe_exit();
#endif

using Isr = void (*)();

static Isr const exception_stubs[32] = {
    isr_exception_0,
    isr_exception_1,
    isr_exception_2,
    isr_exception_3,
    isr_exception_4,
    isr_exception_5,
    isr_exception_6,
    isr_exception_7,
    isr_exception_8,
    isr_exception_9,
    isr_exception_10,
    isr_exception_11,
    isr_exception_12,
    isr_exception_13,
    isr_exception_14,
    isr_exception_15,
    isr_exception_16,
    isr_exception_17,
    isr_exception_18,
    isr_exception_19,
    isr_exception_20,
    isr_exception_21,
    isr_exception_22,
    isr_exception_23,
    isr_exception_24,
    isr_exception_25,
    isr_exception_26,
    isr_exception_27,
    isr_exception_28,
    isr_exception_29,
    isr_exception_30,
    isr_exception_31
};
static Isr const irq_stubs[16] = {
    isr_irq0, isr_irq1, isr_irq2, isr_irq3,
    isr_irq4, isr_irq5, isr_irq6, isr_irq7,
    isr_irq8, isr_irq9, isr_irq10, isr_irq11,
    isr_irq12, isr_irq13, isr_irq14, isr_irq15
};

static void set_idt_gate(
    int num, uint64_t base, uint16_t sel, uint8_t flags,
    uint8_t ist_index = 0
) {
    idt_entries[num].offset_low = static_cast<uint16_t>(base & 0xFFFFu);
    idt_entries[num].selector = sel;
    idt_entries[num].ist = static_cast<uint8_t>(ist_index & 0x07U);
    idt_entries[num].type_attr = flags;
    idt_entries[num].offset_middle = static_cast<uint16_t>((base >> 16) & 0xFFFFu);
    idt_entries[num].offset_high = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFFu);
    idt_entries[num].zero = 0;
}

static uint64_t irq_save_disable() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) asm volatile("sti" ::: "memory");
}

static void remap_pic() {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF); io_wait();
    outb(0xA1, 0xFF); io_wait();
}

static void pic_unmask(uint8_t irq) {
    if (irq >= 8u) {
        uint8_t slave_mask = inb(0xA1);
        slave_mask = static_cast<uint8_t>(slave_mask & ~(1u << (irq - 8u)));
        outb(0xA1, slave_mask);
        uint8_t master_mask = inb(0x21);
        master_mask = static_cast<uint8_t>(master_mask & ~(1u << 2));
        outb(0x21, master_mask);
    } else {
        uint8_t master_mask = inb(0x21);
        master_mask = static_cast<uint8_t>(master_mask & ~(1u << irq));
        outb(0x21, master_mask);
    }
}

bool irq_register_handler(uint8_t irq, IrqHandler handler) {
    if (irq >= 16u || handler == nullptr) return false;
    const uint64_t flags = irq_save_disable();
    irq_handlers[irq] = handler;
    pic_unmask(irq);
    irq_restore(flags);
    return true;
}

uint64_t irq_get_dispatch_count(uint8_t irq) {
    if (irq >= 16u) return 0;
    return __atomic_load_n(&irq_dispatch_counts[irq], __ATOMIC_RELAXED);
}

static void pit_timer_irq_handler(uint8_t irq) {
    (void)irq;
    __atomic_fetch_add(&pit_timer_ticks, 1ULL, __ATOMIC_RELAXED);
}

bool timer_init(uint32_t frequency_hz) {
    if (frequency_hz < 19u || frequency_hz > 1000u) return false;
    if (__atomic_load_n(&pit_timer_ready, __ATOMIC_ACQUIRE) != 0u)
        return __atomic_load_n(&pit_timer_frequency, __ATOMIC_RELAXED) == frequency_hz;

    const uint32_t divisor32 = 1193182u / frequency_hz;
    if (divisor32 == 0u || divisor32 > 65535u) return false;
    const uint16_t divisor = static_cast<uint16_t>(divisor32);
    const uint64_t flags = irq_save_disable();

    /* PIT channel 0, lo/hi byte, mode 3, binary. IRQ0 remains masked until
       irq_register_handler installs the tick callback below. */
    outb(0x43u, 0x36u);
    outb(0x40u, static_cast<uint8_t>(divisor & 0xFFu));
    outb(0x40u, static_cast<uint8_t>((divisor >> 8) & 0xFFu));
    __atomic_store_n(&pit_timer_ticks, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pit_timer_divisor, static_cast<uint32_t>(divisor), __ATOMIC_RELAXED);
    __atomic_store_n(&pit_timer_frequency, frequency_hz, __ATOMIC_RELAXED);

    if (!irq_register_handler(0u, pit_timer_irq_handler)) {
        __atomic_store_n(&pit_timer_ready, 0u, __ATOMIC_RELEASE);
        irq_restore(flags);
        return false;
    }
    __atomic_store_n(&pit_timer_ready, 1u, __ATOMIC_RELEASE);
    irq_restore(flags);

    /* Verify that IRQ0 actually advances the clock. This is a bounded spin,
       not HLT, so a missing interrupt cannot hang early boot. */
    uint32_t spin = 0u;
    while (spin < 5000000u &&
           __atomic_load_n(&pit_timer_ticks, __ATOMIC_ACQUIRE) == 0u) {
        asm volatile("pause" ::: "memory");
        ++spin;
    }
    if (__atomic_load_n(&pit_timer_ticks, __ATOMIC_ACQUIRE) == 0u) {
        __atomic_store_n(&pit_timer_ready, 0u, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

bool timer_is_ready() {
    return __atomic_load_n(&pit_timer_ready, __ATOMIC_ACQUIRE) != 0u;
}

uint64_t timer_now_ms() {
    if (!timer_is_ready()) return 0u;
    const uint64_t ticks = __atomic_load_n(&pit_timer_ticks, __ATOMIC_ACQUIRE);
    const uint32_t divisor = __atomic_load_n(&pit_timer_divisor, __ATOMIC_RELAXED);
    /* Convert PIT input clocks to milliseconds instead of assuming the rounded
       divisor produces an exact 1000-Hz oscillator. */
    return (ticks * static_cast<uint64_t>(divisor) * 1000ULL) / 1193182ULL;
}

bool interrupts_enabled() {
    uint64_t flags = 0u;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) != 0u;
}

static bool pic_irq_is_spurious(uint8_t irq) {
    if (irq == 7u) {
        outb(0x20, 0x0B); // Read master ISR.
        return (inb(0x20) & 0x80u) == 0;
    }
    if (irq == 15u) {
        outb(0xA0, 0x0B); // Read slave ISR.
        return (inb(0xA0) & 0x80u) == 0;
    }
    return false;
}

extern "C" uint64_t starlix_irq_dispatch(
    uint64_t vector, starlix::cpu::InterruptReturnFrame* frame
) {
    if (vector < 0x20u || vector > 0x2Fu) return 0ULL;
    const uint8_t irq = static_cast<uint8_t>(vector - 0x20u);

    if (pic_irq_is_spurious(irq)) {
        /* Spurious IRQ15 came through the slave cascade; acknowledge master only. */
        if (irq == 15u) outb(0x20, 0x20);
        return 0ULL;
    }

    __atomic_fetch_add(&irq_dispatch_counts[irq], 1ULL, __ATOMIC_RELAXED);
    IrqHandler handler = irq_handlers[irq];
    if (handler) handler(irq);

    if (irq >= 8u) outb(0xA0, 0x20);
    outb(0x20, 0x20);

    /* Only a user-originated PIT tick may request a thread switch. The
       coordinator remains disarmed until a loader enters the first validated
       user thread through context_switch_start_first_user(). */
    if (irq == 0u && frame && (frame->cs & 3ULL) == 3ULL) {
        return starlix::process::context_switch::context_switch_on_timer(frame);
    }
    return 0ULL;
}

void idt_init() {
    remap_pic();

    const uint64_t default_address =
        reinterpret_cast<uint64_t>(isr_default_handler);

    for (int i = 0; i < 256; ++i)
        set_idt_gate(i, default_address, 0x08, 0x8E);

    // Dedicated exception stubs and independent IST stacks.
    for (uint32_t vector = 0; vector < 32; ++vector) {
        set_idt_gate(
            static_cast<int>(vector),
            reinterpret_cast<uint64_t>(exception_stubs[vector]),
            0x08,
            0x8E,
            exception_ist_for_vector(vector));
    }

    // Preserve the existing legacy PIC IRQ stubs.
    for (uint8_t irq = 0; irq < 16u; ++irq) {
        set_idt_gate(0x20 + irq,
                     reinterpret_cast<uint64_t>(irq_stubs[irq]), 0x08, 0x8E);
    }
    /* A fixed, restricted CPL3 Lock Screen HostApi gate. The dispatcher only
       accepts the registered stlcs thread and a small versioned service list. */
    set_idt_gate(0x81, reinterpret_cast<uint64_t>(isr_user_service),
                 0x08, 0xEE);
#if STARLIX_ENABLE_RING3_SMOKE_TEST
    /* Test-only DPL3 interrupt gate. It is absent in normal production builds. */
    set_idt_gate(0x80, reinterpret_cast<uint64_t>(isr_user_probe_exit),
                 0x08, 0xEE);
#endif

    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base = reinterpret_cast<uint64_t>(&idt_entries);
    asm_load_idt(reinterpret_cast<uint64_t>(&idt_ptr));
    asm volatile("sti" ::: "memory");
}

bool user_host_service_gate_ready() {
    const IdtEntry& gate = idt_entries[0x81];
    const uint64_t address =
        static_cast<uint64_t>(gate.offset_low) |
        (static_cast<uint64_t>(gate.offset_middle) << 16) |
        (static_cast<uint64_t>(gate.offset_high) << 32);
    return gate.selector == 0x08 && gate.type_attr == 0xEE &&
        gate.ist == 0u && gate.zero == 0u &&
        address == reinterpret_cast<uint64_t>(isr_user_service);
}

bool idt_exception_ist_ready() {
    for (uint32_t vector = 0; vector < 32; ++vector) {
        const IdtEntry& gate = idt_entries[vector];

        const uint64_t address =
            static_cast<uint64_t>(gate.offset_low) |
            (static_cast<uint64_t>(gate.offset_middle) << 16) |
            (static_cast<uint64_t>(gate.offset_high) << 32);

        if (gate.ist != exception_ist_for_vector(vector) ||
            gate.selector != 0x08 ||
            gate.type_attr != 0x8E ||
            gate.zero != 0 ||
            address != reinterpret_cast<uint64_t>(exception_stubs[vector])) {
            return false;
        }
    }

    return true;
}

} // namespace starlix::cpu
