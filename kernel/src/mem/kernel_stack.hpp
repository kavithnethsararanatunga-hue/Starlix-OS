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

namespace starlix::process { struct StThread; }

namespace starlix::mem {

constexpr uint32_t KERNEL_STACK_MAGIC = 0x4B535447u; // "KSTG"
constexpr uint32_t KERNEL_STACK_MIN_PAGES = 4u;
constexpr uint32_t KERNEL_STACK_DEFAULT_PAGES = 4u;
constexpr uint32_t KERNEL_STACK_MAX_PAGES = 16u;
constexpr uint32_t KERNEL_STACK_CAPACITY = 128u;
constexpr uint64_t KERNEL_STACK_PAGE_BYTES = 4096ULL;

enum KernelStackState : uint32_t {
    KERNEL_STACK_FREE = 0u,
    KERNEL_STACK_RESERVED,
    KERNEL_STACK_ALLOCATED,
    KERNEL_STACK_BINDING,
    KERNEL_STACK_BOUND,
    KERNEL_STACK_RELEASING
};

struct KernelStack {
    uint64_t virtual_base;
    uint64_t physical_base;
    uint64_t size;
    uint64_t top;
    uint32_t page_count;
    uint32_t allocation_id;
    uint32_t magic;
    uint32_t reserved;
};

using KernelStackCriticalEnterFn = uint64_t (*)();
using KernelStackCriticalLeaveFn = void (*)(uint64_t saved_flags);
struct KernelStackPlatform {
    KernelStackCriticalEnterFn critical_enter;
    KernelStackCriticalLeaveFn critical_leave;
};

/* The manager is initialized only after phys_init() and address_space_init().
   The hardware entry point installs a single-CPU CLI/restore critical section;
   host tests can install deterministic callbacks. */
bool kernel_stack_manager_init(const KernelStackPlatform& platform);
bool kernel_stack_init();
bool kernel_stack_manager_ready();
const char* kernel_stack_failure_reason();

/* Each stack receives two unmapped guard pages and a contiguous physical run.
   Virtual arena addresses are monotonic; released physical pages are reusable. */
bool kernel_stack_allocate(uint32_t page_count, KernelStack& out_stack);
bool kernel_stack_allocate_default(KernelStack& out_stack);
/* Release an unbound stack (owner_thread=null) or detach and release a stack
   belonging to an unpublished READY thread. Running stacks are never freed. */
bool kernel_stack_release(KernelStack& stack,
                          starlix::process::StThread* owner_thread = nullptr);

/* Bind only after StNtCreateThread() has been given stack.top. This prepares
   its initial user IRETQ frame, records ownership and verifies the shared
   supervisor/NX mapping in that process address space. */
bool kernel_stack_bind_thread(
    const KernelStack& stack, starlix::process::StThread* thread
);

/* Context-switch admission must use this check; caller-owned/unmapped stacks
   are not accepted for first-user entry or timer preemption. */
bool kernel_stack_thread_valid(
    const starlix::process::StThread* thread, uint64_t process_root
);
uint32_t kernel_stack_count();

} // namespace starlix::mem
