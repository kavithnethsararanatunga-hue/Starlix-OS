/*
 * Starlix EHCI Phase 6D-2: idempotent DMA-storage initialization helper.
 *
 * The current kernel has no DMA page-free/unmap operation. When an init attempt
 * partially succeeds, retrying must preserve existing allocations instead of
 * allocating replacements and losing the only CPU pointer to the old page.
 */
#pragma once

namespace starlix::drivers::ehci::lifetime {

template <typename T, typename Allocator>
inline bool ensure_once(T*& object, Allocator allocator) {
    if (object != nullptr) return true;
    T* candidate = allocator();
    if (candidate == nullptr) return false;
    object = candidate;
    return true;
}

/* DMA-backed descriptor/buffer memory may be reused only after the controller
   acknowledged HALTED and both schedule-status bits are inactive. A halt bit
   by itself is not enough evidence to release controller ownership. */
constexpr bool controller_dma_reuse_allowed(bool halt_acknowledged,
                                            bool async_schedule_inactive,
                                            bool periodic_schedule_inactive) {
    return halt_acknowledged && async_schedule_inactive && periodic_schedule_inactive;
}

} // namespace starlix::drivers::ehci::lifetime
