// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdio>
#include "../kernel/src/process/user_process_start_policy.hpp"

using namespace starlix::process;

static unsigned checks = 0;
static unsigned failures = 0;

static void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("[FAIL] %s\n", label);
    } else {
        std::printf("[PASS] %s\n", label);
    }
}

int main() {
    const UserProcessStartFacts valid{
        true, true, true, true, true, true, true, true
    };
    check(user_process_start_allowed(valid),
          "complete prepared process may request its first user entry");

    UserProcessStartFacts facts = valid;
    facts.handle_matches_record = false;
    check(!user_process_start_allowed(facts),
          "forged or stale process handle is rejected");
    facts = valid; facts.transaction_complete = false;
    check(!user_process_start_allowed(facts),
          "incomplete resource transaction is rejected");
    facts = valid; facts.process_ready = false;
    check(!user_process_start_allowed(facts),
          "non-READY process is rejected");
    facts = valid; facts.thread_ready = false;
    check(!user_process_start_allowed(facts),
          "non-READY main thread is rejected");
    facts = valid; facts.scheduler_admitted = false;
    check(!user_process_start_allowed(facts),
          "thread absent from scheduler ready set is rejected");
    facts = valid; facts.context_switch_ready = false;
    check(!user_process_start_allowed(facts),
          "uninitialized context-switch coordinator is rejected");
    facts = valid; facts.context_switch_idle = false;
    check(!user_process_start_allowed(facts),
          "armed or already-running context switch is rejected");
    facts = valid; facts.kernel_address_space_active = false;
    check(!user_process_start_allowed(facts),
          "first-user entry from a non-kernel address space is rejected");

    std::printf("User-process Phase 2M start policy: %u passed, %u failed\n",
                checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
