// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::cpu {

constexpr uint32_t MSR_EFER   = 0xC0000080;
constexpr uint32_t MSR_IA32_PAT = 0x00000277;
constexpr uint64_t EFER_NXE = 1ULL << 11;
constexpr uint32_t MSR_STAR   = 0xC0000081;
constexpr uint32_t MSR_LSTAR  = 0xC0000082;
constexpr uint32_t MSR_SFMASK = 0xC0000084;
constexpr uint32_t MSR_FS_BASE = 0xC0000100;
constexpr uint32_t MSR_GS_BASE = 0xC0000101;
constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;

inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = static_cast<uint32_t>(val);
    uint32_t high = static_cast<uint32_t>(val >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

} // namespace starlix::cpu
