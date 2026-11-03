// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "gdt.hpp"

namespace starlix::cpu::exception_policy {

/* In IA-32e mode, privilege stack switching may leave SS null. A nested
   exception/IST frame can therefore legitimately save SS=0 in kernel mode. */
constexpr bool valid_kernel_ss(uint64_t ss) {
    return ss == 0ULL || ss == KERNEL_DATA_SEGMENT;
}

constexpr bool stack_range_contains(uint64_t rsp, uint64_t begin,
                                    uint64_t size) {
    if (size == 0ULL || begin > (~0ULL - size)) return false;
    const uint64_t end = begin + size;
    return rsp >= begin && rsp <= end;
}

} // namespace starlix::cpu::exception_policy
