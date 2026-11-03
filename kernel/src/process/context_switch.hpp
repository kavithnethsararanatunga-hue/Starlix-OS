// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "process.hpp"
#include "../cpu/interrupt_frame.hpp"

namespace starlix::process::context_switch {

/* Initialize the switch coordinator after process/scheduler initialization.
   It remains disarmed until a loader explicitly enters the first user thread. */
void context_switch_init();
bool context_switch_ready();
bool context_switch_armed();
StThread* context_switch_current();
uint64_t context_switch_switch_count();

/* Validates image/stack mappings, saved frame, FPU state and kernel stack,
   then enters the first user thread with IRETQ. Returns false on any failure. */
bool context_switch_start_first_user(StThread* first_thread);

/* Timer IRQ hook. Returns zero to resume the current frame, or a validated
   alternate frame pointer after a complete CR3/TSS/FPU/thread-state switch. */
uint64_t context_switch_on_timer(
    starlix::cpu::InterruptReturnFrame* current_frame
);

} // namespace starlix::process::context_switch
