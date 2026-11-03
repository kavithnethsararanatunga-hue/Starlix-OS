// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_ENABLE_RING3_SMOKE_TEST
#define STARLIX_ENABLE_RING3_SMOKE_TEST 0
#endif

namespace starlix::process::user_mode_smoke_test {
bool enabled();
const char* failure_reason();
/* Test-only, one-way first-user launch. Successful launch never returns. */
bool start();
}

extern "C" [[noreturn]] void starlix_user_probe_exit_dispatch(
    const void* raw_frame
);
