// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::process {

struct UserProcessStartFacts {
    bool handle_matches_record;
    bool transaction_complete;
    bool process_ready;
    bool thread_ready;
    bool scheduler_admitted;
    bool context_switch_ready;
    bool context_switch_idle;
    bool kernel_address_space_active;
};

constexpr bool user_process_start_allowed(
    const UserProcessStartFacts& facts
) {
    return facts.handle_matches_record &&
           facts.transaction_complete &&
           facts.process_ready &&
           facts.thread_ready &&
           facts.scheduler_admitted &&
           facts.context_switch_ready &&
           facts.context_switch_idle &&
           facts.kernel_address_space_active;
}

} // namespace starlix::process
