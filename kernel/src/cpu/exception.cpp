// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "exception.hpp"
#include "gdt.hpp"
#include "exception_frame_policy.hpp"
#include "io.hpp"
#include "../process/context_switch.hpp"

namespace starlix::cpu {

extern "C" {
alignas(4) uint32_t starlix_exception_active = 0;
}

extern "C" uint8_t __starlix_kernel_start;
extern "C" uint8_t __starlix_kernel_end;
extern "C" uint8_t __starlix_boot_stack_bottom;
extern "C" uint8_t __starlix_boot_stack_top;

static constexpr uint16_t COM1_DATA = 0x3F8;
static constexpr uint16_t COM1_LSR  = 0x3FD;

static uint32_t output_chars_left = 2048;
static uint32_t uart_poll_budget = 100000000;

static const char* const exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "BOUND Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Reserved Exception 9",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved Exception 15",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved Exception 22",
    "Reserved Exception 23",
    "Reserved Exception 24",
    "Reserved Exception 25",
    "Reserved Exception 26",
    "Reserved Exception 27",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved Exception 31"
};

/*
 * Global polling budget bounds the total work spent waiting for UART.
 * Output is also limited by a separate character budget.
 */
static bool serial_putc_bounded(char value) {
    if (output_chars_left == 0 || uart_poll_budget == 0)
        return false;

    while (uart_poll_budget != 0) {
        --uart_poll_budget;

        if ((inb(COM1_LSR) & 0x20u) != 0) {
            outb(COM1_DATA, static_cast<uint8_t>(value));
            --output_chars_left;
            return true;
        }

        asm volatile("pause" ::: "memory");
    }

    return false;
}

static void serial_write_bounded(const char* text) {
    if (!text)
        return;

    while (*text != '\0') {
        if (!serial_putc_bounded(*text))
            return;
        ++text;
    }
}

static void serial_hex64(uint64_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";

    serial_write_bounded("0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        const uint8_t digit =
            static_cast<uint8_t>((value >> shift) & 0x0Fu);

        if (!serial_putc_bounded(digits[digit]))
            return;
    }
}

static void serial_field(const char* label, uint64_t value) {
    serial_write_bounded(label);
    serial_write_bounded("=");
    serial_hex64(value);
    serial_write_bounded("\r\n");
}

static void exception_halt() __attribute__((noreturn));

static void exception_halt() {
    asm volatile("cli" ::: "memory");

    for (;;) {
        asm volatile("hlt" ::: "memory");
    }
}

/*
 * Do not dereference frame until its complete range is verified
 * inside one of the four dedicated IST stacks.
 */
static uint8_t identify_frame_ist(const ExceptionFrame* frame) {
    const uint64_t address =
        reinterpret_cast<uint64_t>(frame);

    for (uint8_t ist = 1; ist <= 4; ++ist) {
        if (tss_exception_stack_contains(
                ist, address, sizeof(ExceptionFrame))) {
            return ist;
        }
    }

    return 0;
}

static const char* validate_frame(
    const ExceptionFrame& frame,
    uint8_t actual_ist
) {
    if (frame.vector >= 32)
        return "Invalid exception vector";

    if (actual_ist != exception_ist_for_vector(frame.vector))
        return "Unexpected IST stack";

    if (frame.cs != KERNEL_CODE_SEGMENT)
        return "Unexpected kernel CS";

    if (!exception_policy::valid_kernel_ss(frame.ss))
        return "Unexpected kernel SS";

    if ((frame.rflags & 0x2ULL) == 0)
        return "RFLAGS reserved bit 1 is clear";

    const uint64_t kernel_begin =
        reinterpret_cast<uint64_t>(&__starlix_kernel_start);

    const uint64_t kernel_end =
        reinterpret_cast<uint64_t>(&__starlix_kernel_end);

    if (kernel_end <= kernel_begin ||
        frame.rip < kernel_begin ||
        frame.rip >= kernel_end) {
        return "RIP outside linked kernel image";
    }

    uint64_t stack_begin =
        reinterpret_cast<uint64_t>(&__starlix_boot_stack_bottom);
    uint64_t stack_size =
        reinterpret_cast<uint64_t>(&__starlix_boot_stack_top) - stack_begin;

    /* After first IRETQ, synchronous faults can occur while the kernel is
       running on the current user thread's guarded RSP0 stack, not the boot
       stack. The coordinator pointer is a trusted static kernel-owned record. */
    const starlix::process::StThread* current =
        starlix::process::context_switch::context_switch_current();
    if (current) {
        stack_begin = current->kernel_stack_base;
        stack_size = current->kernel_stack_size;
    }

    if (!exception_policy::stack_range_contains(
            frame.rsp, stack_begin, stack_size)) {
        return "Interrupted RSP outside active kernel stack";
    }

    return nullptr;
}

} // namespace starlix::cpu

extern "C" void starlix_exception_dispatch(
    const starlix::cpu::ExceptionFrame* frame
) {
    using namespace starlix::cpu;

    const uint8_t actual_ist = identify_frame_ist(frame);

    if (actual_ist == 0) {
        serial_write_bounded(
            "\r\n[EXCEPTION] FATAL: invalid IST frame address.\r\n");
        exception_halt();
    }

    const ExceptionFrame& f = *frame;
    const char* validation_error =
        validate_frame(f, actual_ist);

    uint64_t cr2 = 0;

    if (f.vector == 14) {
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    serial_write_bounded(
        "\r\n========== STARLIX FATAL CPU EXCEPTION ==========\r\n");

    serial_field("VECTOR", f.vector);

    if (f.vector < 32) {
        serial_write_bounded("NAME=");
        serial_write_bounded(exception_names[f.vector]);
        serial_write_bounded("\r\n");
    } else {
        serial_write_bounded("NAME=Invalid exception vector\r\n");
    }

    serial_field("IST", actual_ist);
    serial_field("ERROR_CODE", f.error_code);

    serial_write_bounded(
        exception_vector_has_error_code(f.vector)
            ? "ERROR_CODE_SOURCE=CPU\r\n"
            : "ERROR_CODE_SOURCE=SYNTHETIC_ZERO\r\n");

    serial_field("RIP", f.rip);
    serial_field("CS", f.cs);
    serial_field("RFLAGS", f.rflags);
    serial_field("RSP", f.rsp);
    serial_field("SS", f.ss);

    if (f.vector == 14)
        serial_field("CR2_PAGE_FAULT_ADDRESS", cr2);

    serial_write_bounded("---- GENERAL REGISTERS ----\r\n");

    serial_field("RAX", f.rax);
    serial_field("RBX", f.rbx);
    serial_field("RCX", f.rcx);
    serial_field("RDX", f.rdx);
    serial_field("RBP", f.rbp);
    serial_field("RSI", f.rsi);
    serial_field("RDI", f.rdi);
    serial_field("R8", f.r8);
    serial_field("R9", f.r9);
    serial_field("R10", f.r10);
    serial_field("R11", f.r11);
    serial_field("R12", f.r12);
    serial_field("R13", f.r13);
    serial_field("R14", f.r14);
    serial_field("R15", f.r15);

    if (validation_error) {
        serial_write_bounded("FRAME_VALIDATION=FAIL\r\nREASON=");
        serial_write_bounded(validation_error);
        serial_write_bounded("\r\n");
    } else {
        serial_write_bounded("FRAME_VALIDATION=PASS\r\n");
    }

    serial_write_bounded(
        "ACTION=FATAL_FAIL_STOP; NO_EXCEPTION_RETURN\r\n");

    serial_write_bounded(
        "=================================================\r\n");

    exception_halt();
}
