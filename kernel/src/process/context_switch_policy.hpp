// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "process.hpp"
#include "../cpu/interrupt_frame.hpp"
#include "../mem/user_address.hpp"

namespace starlix::process::context_switch {

constexpr uint64_t MIN_KERNEL_STACK_BYTES = 4096ULL;
constexpr uint64_t MAX_KERNEL_STACK_BYTES = 1024ULL * 1024ULL;

inline bool valid_saved_user_frame_bounds(
    const StThread& thread, uint64_t frame_address
) {
    using namespace starlix::mem::user_address;
    using starlix::cpu::INTERRUPT_RETURN_FRAME_SIZE;

    if (thread.kernel_stack_base == 0ULL ||
        !page_aligned(thread.kernel_stack_base) ||
        thread.kernel_stack_size < MIN_KERNEL_STACK_BYTES ||
        thread.kernel_stack_size > MAX_KERNEL_STACK_BYTES ||
        !page_aligned(thread.kernel_stack_size) ||
        thread.kernel_stack_base > (~0ULL - thread.kernel_stack_size)) {
        return false;
    }

    const uint64_t expected_top =
        thread.kernel_stack_base + thread.kernel_stack_size;
    if (thread.kernel_stack_top != expected_top ||
        (expected_top & 0xFULL) != 0ULL ||
        !is_canonical_48(thread.kernel_stack_base) ||
        !is_canonical_48(expected_top - 1ULL) ||
        is_user_address(thread.kernel_stack_base) ||
        is_user_address(expected_top - 1ULL) ||
        (frame_address & 0x7ULL) != 0ULL ||
        frame_address < thread.kernel_stack_base ||
        frame_address > expected_top - INTERRUPT_RETURN_FRAME_SIZE) {
        return false;
    }

    const auto* frame = reinterpret_cast<const starlix::cpu::InterruptReturnFrame*>(
        static_cast<uintptr_t>(frame_address));
    return frame->vector == starlix::cpu::INTERRUPT_FRAME_VECTOR_TIMER &&
           starlix::cpu::valid_user_interrupt_return_frame(*frame);
}

inline bool valid_thread_switch_state(const StThread& thread) {
    if (thread.tid == 0ULL || thread.state != THREAD_STATE_READY ||
        !thread.parent_process || thread.priority > 31u ||
        thread.parent_process->state == PROCESS_STATE_TERMINATED ||
        thread.parent_process->state == PROCESS_STATE_CREATED ||
        thread.parent_process->state == PROCESS_STATE_SYSTEM ||
        thread.parent_process->page_directory_cr3 == 0ULL ||
        (thread.parent_process->page_directory_cr3 & 0xFFFULL) != 0ULL ||
        thread.saved_interrupt_rsp == 0ULL) {
        return false;
    }
    return valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp);
}

} // namespace starlix::process::context_switch
