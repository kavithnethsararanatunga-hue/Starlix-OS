// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "fpu.hpp"

namespace starlix::cpu {
namespace {

constexpr uint64_t CR0_MP = 1ULL << 1;
constexpr uint64_t CR0_EM = 1ULL << 2;
constexpr uint64_t CR0_TS = 1ULL << 3;
constexpr uint64_t CR0_NE = 1ULL << 5;
constexpr uint64_t CR4_OSFXSR = 1ULL << 9;
constexpr uint64_t CR4_OSXMMEXCPT = 1ULL << 10;
constexpr uint64_t CR4_FSGSBASE = 1ULL << 16;
constexpr uint64_t CR4_OSXSAVE = 1ULL << 18;
constexpr uint32_t CPUID_FXSR = 1u << 24;
constexpr uint32_t CPUID_SSE = 1u << 25;
constexpr uint32_t CPUID_SSE2 = 1u << 26;
constexpr uint32_t DEFAULT_MXCSR = 0x1F80u;

alignas(16) static uint8_t initial_state[FPU_STATE_BYTES]{};
static volatile uint32_t initialized = 0u;

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t& eax, uint32_t& ebx,
                  uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf));
}

static bool interrupts_are_disabled() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) == 0ULL;
}

static bool aligned_state(const FpuState& state) {
    return (reinterpret_cast<uintptr_t>(state.bytes) & 0xFu) == 0ULL;
}

} // namespace

bool fpu_init() {
    if (!interrupts_are_disabled()) return false;

    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    cpuid(0u, 0u, eax, ebx, ecx, edx);
    if (eax < 1u) return false;
    cpuid(1u, 0u, eax, ebx, ecx, edx);
    if ((edx & (CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) !=
        (CPUID_FXSR | CPUID_SSE | CPUID_SSE2)) {
        return false;
    }

    uint64_t cr0 = 0ULL;
    uint64_t cr4 = 0ULL;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* Do not use lazy TS/#NM switching. The IRQ path eagerly saves/restores. */
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    /* AVX and unmediated user FS/GS writes remain disabled in this phase. */
    cr4 &= ~(CR4_OSXSAVE | CR4_FSGSBASE);

    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    asm volatile("fninit" ::: "memory");
    const uint32_t mxcsr = DEFAULT_MXCSR;
    asm volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
    /* Do not leak firmware/kernel XMM contents into the first user thread. */
    asm volatile(
        "pxor %%xmm0, %%xmm0\n\t"
        "pxor %%xmm1, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm2\n\t"
        "pxor %%xmm3, %%xmm3\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "pxor %%xmm5, %%xmm5\n\t"
        "pxor %%xmm6, %%xmm6\n\t"
        "pxor %%xmm7, %%xmm7\n\t"
        "pxor %%xmm8, %%xmm8\n\t"
        "pxor %%xmm9, %%xmm9\n\t"
        "pxor %%xmm10, %%xmm10\n\t"
        "pxor %%xmm11, %%xmm11\n\t"
        "pxor %%xmm12, %%xmm12\n\t"
        "pxor %%xmm13, %%xmm13\n\t"
        "pxor %%xmm14, %%xmm14\n\t"
        "pxor %%xmm15, %%xmm15"
        ::: "memory");

    asm volatile("fxsave64 %0" : "=m"(initial_state) :: "memory");
    __atomic_store_n(&initialized, 1u, __ATOMIC_RELEASE);
    return true;
}

bool fpu_ready() {
    return __atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 0u;
}

bool fpu_state_reset(FpuState& state) {
    if (!fpu_ready() || !aligned_state(state)) return false;
    for (uint32_t i = 0; i < FPU_STATE_BYTES; ++i)
        state.bytes[i] = initial_state[i];
    state.initialized = 1u;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

bool fpu_state_save(FpuState& state) {
    if (!fpu_ready() || !aligned_state(state)) return false;
    asm volatile("fxsave64 %0" : "=m"(state.bytes) :: "memory");
    state.initialized = 1u;
    return true;
}

bool fpu_state_restore(const FpuState& state) {
    if (!fpu_ready() || !aligned_state(state) || state.initialized != 1u)
        return false;
    asm volatile("fxrstor64 %0" :: "m"(state.bytes) : "memory");
    return true;
}

} // namespace starlix::cpu
