// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::cpu {

constexpr uint16_t KERNEL_CODE_SEGMENT = 0x08;
constexpr uint16_t KERNEL_DATA_SEGMENT = 0x10;
constexpr uint16_t USER_DATA_SEGMENT   = 0x18 | 3;
constexpr uint16_t USER_CODE_SEGMENT   = 0x20 | 3;
constexpr uint16_t TSS_SEGMENT         = 0x28;

void gdt_init();
void tss_set_kernel_stack(uint64_t stack_ptr);
bool tss_set_kernel_stack_checked(uint64_t stack_ptr);
uint64_t tss_get_kernel_stack();
bool tss_exception_stacks_ready();
bool tss_exception_stack_contains(
    uint8_t ist_index, uint64_t address, size_t length
);

} // namespace starlix::cpu
