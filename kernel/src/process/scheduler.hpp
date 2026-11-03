// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "process.hpp"

namespace starlix::process::scheduler {

constexpr uint32_t MAX_READY_THREADS = 64u;
constexpr uint32_t PRIORITY_LEVELS = 32u;
constexpr uint32_t MIN_PRIORITY = 0u;
constexpr uint32_t MAX_PRIORITY = PRIORITY_LEVELS - 1u;

/* Initialize the bounded ready set. Call after process_init(). */
void scheduler_init();
bool scheduler_ready();

/* Admit a registered READY thread exactly once. Priorities 0..31 are valid;
   larger values are rejected. The scheduler does not own the thread object. */
bool scheduler_admit(StThread* thread);
bool scheduler_remove(uint64_t tid);
/* True only when this exact registered READY thread owns a ready-set slot. */
bool scheduler_is_admitted(const StThread* thread);
bool scheduler_set_priority(StThread* thread, uint32_t priority);

/* Selects the next READY thread using strict priority + round-robin among
   equal priorities. This is a policy decision only: it does NOT change thread
   or process states, load CR3, change TSS.RSP0, or perform a context switch. */
StThread* scheduler_pick_next();
uint32_t scheduler_ready_count();

} // namespace starlix::process::scheduler
