// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "msr.hpp"

namespace starlix::cpu {

using IrqHandler = void (*)(uint8_t irq);

void idt_init();
bool idt_exception_ist_ready();
/* Restricted CPL3 Lock Screen HostApi gate at INT 0x81. */
bool user_host_service_gate_ready();

/* Legacy 8259 PIC IRQ registration. IRQ handlers run with IF cleared and
   must be short, non-blocking, and must not call serial/allocator code. */
bool irq_register_handler(uint8_t irq, IrqHandler handler);
uint64_t irq_get_dispatch_count(uint8_t irq);

/* Legacy PIT channel-0 monotonic clock. timer_now_ms() has approximately
   millisecond resolution; initialize after idt_init(). IRQ handler is minimal. */
bool timer_init(uint32_t frequency_hz = 1000u);
bool timer_is_ready();
uint64_t timer_now_ms();
bool interrupts_enabled();

} // namespace starlix::cpu
