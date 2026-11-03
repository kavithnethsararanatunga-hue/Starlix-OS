// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "msr.hpp"

namespace starlix::cpu {

typedef uint64_t (*SyscallServiceFn)(uint64_t, uint64_t, uint64_t);

bool syscall_init();
void syscall_table_init();

} // namespace starlix::cpu
