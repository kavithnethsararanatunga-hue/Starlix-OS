// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "gdt.hpp"
#include "../mem/user_address.hpp"

namespace starlix::cpu {

/*
 * Exact stack layout produced by idt_asm.S when a Ring-3 interrupt arrives:
 *
 *   GPRs (15 qwords), vector, RIP, CS, RFLAGS, user RSP, user SS.
 *
 * The IRQ common stub may return a pointer to another frame with this same
 * layout. Its epilogue restores the GPRs, discards vector, then executes IRETQ.
 * This is a contract only; scheduling remains disabled until a user thread is
 * explicitly launched and registered as current.
 */
struct InterruptReturnFrame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t vector;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

constexpr uint64_t INTERRUPT_RETURN_FRAME_SIZE =
    sizeof(InterruptReturnFrame);
constexpr uint64_t INTERRUPT_FRAME_VECTOR_TIMER = 32ULL;

static_assert(sizeof(InterruptReturnFrame) == 168ULL,
              "IRQ restore frame must match idt_asm.S push/pop layout");
static_assert(__builtin_offsetof(InterruptReturnFrame, vector) == 120ULL);
static_assert(__builtin_offsetof(InterruptReturnFrame, rip) == 128ULL);
static_assert(__builtin_offsetof(InterruptReturnFrame, cs) == 136ULL);
static_assert(__builtin_offsetof(InterruptReturnFrame, rflags) == 144ULL);
static_assert(__builtin_offsetof(InterruptReturnFrame, rsp) == 152ULL);
static_assert(__builtin_offsetof(InterruptReturnFrame, ss) == 160ULL);

constexpr bool valid_user_interrupt_return_frame(
    const InterruptReturnFrame& frame
) {
    using namespace starlix::mem::user_address;
    constexpr uint64_t RFLAGS_FIXED = 1ULL << 1;
    constexpr uint64_t RFLAGS_IOPL = 3ULL << 12;
    constexpr uint64_t RFLAGS_NT = 1ULL << 14;
    constexpr uint64_t RFLAGS_VM = 1ULL << 17;

    /* A live CPL3 interrupt may capture RSP at any byte/8-byte alignment.
       The SysV ABI aligns RSP at call boundaries, not at arbitrary INT/IRQ
       boundaries. Only the initial user IRETQ frame requires 16-byte alignment. */
    const bool user_stack_address =
        is_user_address(frame.rsp) || frame.rsp == USER_REGION_END;

    return frame.cs == USER_CODE_SEGMENT &&
           frame.ss == USER_DATA_SEGMENT &&
           is_user_address(frame.rip) &&
           user_stack_address &&
           (frame.rflags & RFLAGS_FIXED) != 0ULL &&
           (frame.rflags & (RFLAGS_IOPL | RFLAGS_NT | RFLAGS_VM)) == 0ULL;
}

/* The first IRETQ entry starts a fresh ABI-compliant user stack. Keep this
   stricter check separate from validation of live interrupt-return frames. */
constexpr bool valid_initial_user_interrupt_return_frame(
    const InterruptReturnFrame& frame
) {
    using namespace starlix::mem::user_address;
    return valid_user_interrupt_return_frame(frame) &&
           is_user_stack_pointer(frame.rsp);
}

inline void build_initial_user_interrupt_frame(
    InterruptReturnFrame& frame,
    uint64_t entry_point,
    uint64_t user_stack
) {
    auto* bytes = reinterpret_cast<uint8_t*>(&frame);
    for (uint64_t i = 0; i < sizeof(frame); ++i) bytes[i] = 0;
    frame.vector = INTERRUPT_FRAME_VECTOR_TIMER;
    frame.rip = entry_point;
    frame.cs = USER_CODE_SEGMENT;
    frame.rflags = 0x202ULL;
    frame.rsp = user_stack;
    frame.ss = USER_DATA_SEGMENT;
}

} // namespace starlix::cpu
