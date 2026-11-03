// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../ob/object.hpp"
#include "../cpu/fpu.hpp"

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::process {

constexpr uint32_t MAX_PROCESSES = 32;
constexpr uint32_t MAX_THREADS = 128;
constexpr uint32_t MAX_THREADS_PER_PROCESS = 16;
constexpr uint64_t SYSTEM_PROCESS_ID = 4;

enum ProcessState : uint32_t {
    PROCESS_STATE_FREE = 0,
    PROCESS_STATE_SYSTEM,
    PROCESS_STATE_CREATED,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_TERMINATED
};

enum ThreadState : uint32_t {
    THREAD_STATE_FREE = 0,
    THREAD_STATE_INITIALIZED,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_TERMINATED
};

struct ThreadContext {
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

struct StProcess;

struct StThread {
    uint64_t tid;
    uint32_t priority;
    ThreadState state;
    ThreadContext context;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_size;
    uint64_t kernel_stack_physical_base;
    uint32_t kernel_stack_allocation_id;
    uint32_t kernel_stack_reserved;
    uint64_t saved_interrupt_rsp;
    uint64_t user_stack;
    uint64_t fs_base;
    uint64_t user_gs_base;
    starlix::cpu::FpuState fpu_state;
    StProcess* parent_process;
};

struct StProcess {
    uint64_t pid;
    char image_name[32];
    uint64_t page_directory_cr3;
    starlix::ob::HANDLE handle_table[64];
    StThread* main_thread;
    uint32_t thread_count;
    ProcessState state;
};

void process_init();
StProcess* process_system();
StProcess* process_find(uint64_t pid);
bool process_is_registered(const StProcess* process);
StThread* thread_find(uint64_t tid);
bool thread_is_registered(const StThread* thread);
/* Prepare the first Ring-3 IRETQ frame inside a caller-owned kernel stack.
   This does not launch the thread or enable scheduler preemption. */
bool thread_prepare_user_interrupt_frame(
    StThread* thread, uint64_t kernel_stack_base, uint64_t kernel_stack_size
);
uint64_t thread_saved_interrupt_frame(const StThread* thread);
StProcess* StNtCreateProcess(const char* name, uint64_t cr3);
StThread* StNtCreateThread(
    StProcess* process,
    uint64_t entry_point,
    uint64_t user_stack,
    uint64_t kernel_stack_top
);
/* Roll back only the most recently-created, unpublished process and optional
   main thread. The caller must first remove any scheduler admission and detach
   or release the thread's kernel stack. */
bool process_abort_creation(StProcess* process, StThread* main_thread);
bool process_set_state(StProcess* process, ProcessState next_state);
bool thread_set_state(StThread* thread, ThreadState next_state);
uint32_t process_count();
uint32_t thread_count();

} // namespace starlix::process
