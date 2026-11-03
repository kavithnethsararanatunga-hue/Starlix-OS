// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>

extern "C" {

// Fast System Call Assembly Wrapper
static uint64_t starlix_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile (
        "mov %1, %%rax \n"
        "mov %2, %%rdi \n"
        "mov %3, %%rsi \n"
        "mov %4, %%rdx \n"
        "syscall \n"
        "mov %%rax, %0 \n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

// Exported Library Functions for User Applications (.ect)
uint64_t StarlixYield() {
    return starlix_syscall(1, 0, 0, 0);
}

uint64_t StarlixDisplayString(const char* msg) {
    return starlix_syscall(2, reinterpret_cast<uint64_t>(msg), 0, 0);
}

uint64_t StarlixAllocateMemory(uint64_t size) {
    return starlix_syscall(3, size, 0, 0);
}

} // extern "C"
