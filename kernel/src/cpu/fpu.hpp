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

constexpr uint32_t FPU_STATE_BYTES = 512u;

struct alignas(16) FpuState {
    alignas(16) uint8_t bytes[FPU_STATE_BYTES];
    uint8_t initialized;
};

static_assert(__builtin_offsetof(FpuState, bytes) == 0u);
static_assert((__builtin_offsetof(FpuState, bytes) & 0xFu) == 0u);
static_assert(sizeof(((FpuState*)nullptr)->bytes) == FPU_STATE_BYTES);

/* Enables legacy x87/MMX/SSE state only. AVX/XSAVE remains disabled. */
bool fpu_init();
bool fpu_ready();
bool fpu_state_reset(FpuState& state);
bool fpu_state_save(FpuState& state);
bool fpu_state_restore(const FpuState& state);

} // namespace starlix::cpu
