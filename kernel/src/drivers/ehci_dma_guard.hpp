/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once

#include "ehci_spec.hpp"

namespace starlix::drivers::ehci::dma {

constexpr uintptr_t kMaxDmaAddress32 = 0xFFFFFFFFULL;

constexpr bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/* Pure check, intentionally separated so boundary conditions can be tested
   without constructing host pointers or touching hardware. */
constexpr bool identity_range_fits32(uintptr_t address,
                                     size_t bytes,
                                     size_t alignment) {
    if (address == 0 || bytes == 0 || !is_power_of_two(alignment)) return false;
    if ((address & static_cast<uintptr_t>(alignment - 1)) != 0) return false;
    if (address > kMaxDmaAddress32) return false;
    /* Subtraction form avoids overflow in address + bytes - 1. */
    return static_cast<uintptr_t>(bytes - 1) <= kMaxDmaAddress32 - address;
}

/* EHCI is configured for 32-bit segment-0 DMA. Current Starlix static DMA
   buffers are identity-addressed; reject rather than truncate invalid ranges. */
inline bool to_identity_address32(const void* pointer,
                                  size_t bytes,
                                  size_t alignment,
                                  uint32_t& result) {
    if (!pointer) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (!identity_range_fits32(address, bytes, alignment)) return false;
    result = static_cast<uint32_t>(address);
    return true;
}

/* On x86, EHCI DMA is coherent on the platforms targeted by this prototype.
   These fences order ordinary cached-memory descriptor/buffer accesses around
   device ownership. They do not replace physical mapping or cache maintenance
   on non-coherent architectures. */
inline void write_barrier() {
    __asm__ __volatile__("sfence" ::: "memory");
}

inline void read_barrier() {
    __asm__ __volatile__("lfence" ::: "memory");
}

inline void full_barrier() {
    __asm__ __volatile__("mfence" ::: "memory");
}

} // namespace starlix::drivers::ehci::dma
