// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "msr.hpp"

namespace starlix::cpu {

struct ExceptionFrame {
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
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static_assert(sizeof(ExceptionFrame) == 176);
static_assert(__builtin_offsetof(ExceptionFrame, vector) == 120);
static_assert(__builtin_offsetof(ExceptionFrame, error_code) == 128);
static_assert(__builtin_offsetof(ExceptionFrame, rip) == 136);
static_assert(__builtin_offsetof(ExceptionFrame, cs) == 144);
static_assert(__builtin_offsetof(ExceptionFrame, rflags) == 152);
static_assert(__builtin_offsetof(ExceptionFrame, rsp) == 160);
static_assert(__builtin_offsetof(ExceptionFrame, ss) == 168);

constexpr uint8_t exception_ist_for_vector(uint64_t vector) {
    if (vector == 2) return 1;
    if (vector == 8) return 2;
    if (vector == 18) return 3;
    if (vector < 32) return 4;
    return 0;
}

constexpr bool exception_vector_has_error_code(uint64_t vector) {
    switch (vector) {
        case 8:
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 17:
        case 21:
        case 29:
        case 30:
            return true;
        default:
            return false;
    }
}

static_assert(exception_ist_for_vector(2) == 1);
static_assert(exception_ist_for_vector(8) == 2);
static_assert(exception_ist_for_vector(18) == 3);
static_assert(exception_ist_for_vector(14) == 4);
static_assert(exception_ist_for_vector(31) == 4);

static_assert(exception_vector_has_error_code(8));
static_assert(exception_vector_has_error_code(14));
static_assert(exception_vector_has_error_code(17));
static_assert(exception_vector_has_error_code(21));
static_assert(exception_vector_has_error_code(29));
static_assert(exception_vector_has_error_code(30));
static_assert(!exception_vector_has_error_code(2));
static_assert(!exception_vector_has_error_code(18));
static_assert(!exception_vector_has_error_code(20)); // Intel #VE
static_assert(!exception_vector_has_error_code(31));

} // namespace starlix::cpu

extern "C" void starlix_exception_dispatch(
    const starlix::cpu::ExceptionFrame* frame
) __attribute__((noreturn));
