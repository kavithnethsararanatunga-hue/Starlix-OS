// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "process.hpp"
#include "../cpu/gdt.hpp"
#include "../mem/user_address.hpp"
#include "../cpu/interrupt_frame.hpp"

namespace starlix::process {

static StProcess process_table[MAX_PROCESSES]{};
static StThread thread_table[MAX_THREADS]{};
static uint64_t next_pid = SYSTEM_PROCESS_ID + 1ULL;
static uint64_t next_tid = 1ULL;
static uint32_t used_processes = 0;
static uint32_t used_threads = 0;
static uint32_t registry_lock = 0;
static bool registry_ready = false;

static void lock_registry() {
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(
                &registry_lock, &expected, 1u, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        asm volatile("pause" ::: "memory");
    }
}

static void unlock_registry() {
    __atomic_store_n(&registry_lock, 0u, __ATOMIC_RELEASE);
}

static bool valid_process_pointer(const StProcess* process) {
    if (!process) return false;
    for (uint32_t i = 0; i < used_processes; ++i) {
        if (&process_table[i] == process &&
            process_table[i].state != PROCESS_STATE_FREE) {
            return true;
        }
    }
    return false;
}

static bool valid_thread_pointer(const StThread* thread) {
    if (!thread) return false;
    for (uint32_t i = 0; i < used_threads; ++i) {
        if (&thread_table[i] == thread &&
            thread_table[i].state != THREAD_STATE_FREE) {
            return true;
        }
    }
    return false;
}

static bool copy_image_name(char (&destination)[32], const char* source) {
    if (!source) return false;
    uint32_t length = 0;
    while (length < 32u && source[length] != '\0') ++length;
    if (length == 0u || length >= 32u) return false;
    for (uint32_t i = 0; i < length; ++i)
        destination[i] = source[i];
    destination[length] = '\0';
    return true;
}

static void clear_process(StProcess& process) {
    auto* bytes = reinterpret_cast<uint8_t*>(&process);
    for (size_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

static void clear_thread(StThread& thread) {
    auto* bytes = reinterpret_cast<uint8_t*>(&thread);
    for (size_t i = 0; i < sizeof(thread); ++i) bytes[i] = 0;
}

void process_init() {
    lock_registry();
    for (uint32_t i = 0; i < MAX_PROCESSES; ++i)
        clear_process(process_table[i]);
    for (uint32_t i = 0; i < MAX_THREADS; ++i)
        clear_thread(thread_table[i]);

    used_processes = 1u;
    used_threads = 0u;
    next_pid = SYSTEM_PROCESS_ID + 1ULL;
    next_tid = 1ULL;

    StProcess& system = process_table[0];
    system.pid = SYSTEM_PROCESS_ID;
    system.page_directory_cr3 = 0ULL; // Kernel address space, not a user CR3.
    system.state = PROCESS_STATE_SYSTEM;
    system.image_name[0] = 'S';
    system.image_name[1] = 'y';
    system.image_name[2] = 's';
    system.image_name[3] = 't';
    system.image_name[4] = 'e';
    system.image_name[5] = 'm';
    system.image_name[6] = '\0';

    registry_ready = true;
    unlock_registry();
}

StProcess* process_system() {
    lock_registry();
    StProcess* result = registry_ready ? &process_table[0] : nullptr;
    unlock_registry();
    return result;
}

StProcess* process_find(uint64_t pid) {
    lock_registry();
    StProcess* result = nullptr;
    if (registry_ready) {
        for (uint32_t i = 0; i < used_processes; ++i) {
            if (process_table[i].pid == pid &&
                process_table[i].state != PROCESS_STATE_FREE) {
                result = &process_table[i];
                break;
            }
        }
    }
    unlock_registry();
    return result;
}

bool process_is_registered(const StProcess* process) {
    lock_registry();
    const bool result = registry_ready && valid_process_pointer(process);
    unlock_registry();
    return result;
}

StThread* thread_find(uint64_t tid) {
    lock_registry();
    StThread* result = nullptr;
    if (registry_ready) {
        for (uint32_t i = 0; i < used_threads; ++i) {
            if (thread_table[i].tid == tid &&
                thread_table[i].state != THREAD_STATE_FREE) {
                result = &thread_table[i];
                break;
            }
        }
    }
    unlock_registry();
    return result;
}

bool thread_is_registered(const StThread* thread) {
    lock_registry();
    bool result = false;
    if (registry_ready && thread) {
        for (uint32_t i = 0; i < used_threads; ++i) {
            if (&thread_table[i] == thread &&
                thread_table[i].state != THREAD_STATE_FREE) {
                result = true;
                break;
            }
        }
    }
    unlock_registry();
    return result;
}

bool thread_prepare_user_interrupt_frame(
    StThread* thread, uint64_t kernel_stack_base, uint64_t kernel_stack_size
) {
    using namespace starlix::mem::user_address;
    using starlix::cpu::InterruptReturnFrame;
    using starlix::cpu::INTERRUPT_RETURN_FRAME_SIZE;

    if (!page_aligned(kernel_stack_base) ||
        !page_aligned(kernel_stack_size) ||
        kernel_stack_size < PAGE_SIZE ||
        kernel_stack_size > (1024ULL * 1024ULL) ||
        kernel_stack_base > (~0ULL - kernel_stack_size)) {
        return false;
    }
    const uint64_t kernel_stack_top = kernel_stack_base + kernel_stack_size;
    if ((kernel_stack_top & 0xFULL) != 0ULL ||
        !is_canonical_48(kernel_stack_base) ||
        !is_canonical_48(kernel_stack_top - 1ULL) ||
        is_user_address(kernel_stack_base) ||
        is_user_address(kernel_stack_top - 1ULL) ||
        kernel_stack_size < (INTERRUPT_RETURN_FRAME_SIZE + 256ULL)) {
        return false;
    }

    lock_registry();
    if (!registry_ready || !valid_thread_pointer(thread) ||
        thread->state != THREAD_STATE_READY ||
        thread->kernel_stack_top != kernel_stack_top ||
        thread->saved_interrupt_rsp != 0ULL ||
        !valid_process_pointer(thread->parent_process) ||
        thread->parent_process->state == PROCESS_STATE_TERMINATED ||
        !is_user_address(thread->context.rip) ||
        !is_user_stack_pointer(thread->context.rsp)) {
        unlock_registry();
        return false;
    }

    /* Prepared kernel stacks must not overlap another live thread's stack. */
    for (uint32_t i = 0; i < used_threads; ++i) {
        const StThread& other = thread_table[i];
        if (&other == thread || other.state == THREAD_STATE_FREE ||
            other.kernel_stack_size == 0ULL) {
            continue;
        }
        const uint64_t other_end = other.kernel_stack_base +
                                   other.kernel_stack_size;
        if (kernel_stack_base < other_end &&
            other.kernel_stack_base < kernel_stack_top) {
            unlock_registry();
            return false;
        }
    }

    const uint64_t frame_address =
        kernel_stack_top - INTERRUPT_RETURN_FRAME_SIZE;
    if ((frame_address & 0x7ULL) != 0ULL ||
        frame_address < kernel_stack_base ||
        frame_address > kernel_stack_top - sizeof(InterruptReturnFrame)) {
        unlock_registry();
        return false;
    }

    auto* frame = reinterpret_cast<InterruptReturnFrame*>(
        static_cast<uintptr_t>(frame_address));
    starlix::cpu::build_initial_user_interrupt_frame(
        *frame, thread->context.rip, thread->context.rsp);
    if (!starlix::cpu::valid_initial_user_interrupt_return_frame(*frame)) {
        unlock_registry();
        return false;
    }

    thread->kernel_stack_base = kernel_stack_base;
    thread->kernel_stack_size = kernel_stack_size;
    thread->saved_interrupt_rsp = frame_address;
    unlock_registry();
    return true;
}

uint64_t thread_saved_interrupt_frame(const StThread* thread) {
    lock_registry();
    uint64_t result = 0ULL;
    if (registry_ready && valid_thread_pointer(thread))
        result = thread->saved_interrupt_rsp;
    unlock_registry();
    return result;
}

StProcess* StNtCreateProcess(const char* name, uint64_t cr3) {
    if (!name || cr3 == 0ULL || (cr3 & 0xFFFULL) != 0ULL ||
        (cr3 & ~0x000FFFFFFFFFF000ULL) != 0ULL) {
        return nullptr;
    }

    char checked_name[32]{};
    if (!copy_image_name(checked_name, name)) return nullptr;

    lock_registry();
    if (!registry_ready || used_processes >= MAX_PROCESSES ||
        next_pid == 0ULL || next_pid == ~0ULL) {
        unlock_registry();
        return nullptr;
    }

    StProcess& process = process_table[used_processes++];
    clear_process(process);
    process.pid = next_pid++;
    process.page_directory_cr3 = cr3;
    process.state = PROCESS_STATE_CREATED;
    for (uint32_t i = 0; i < sizeof(process.image_name); ++i)
        process.image_name[i] = checked_name[i];
    unlock_registry();
    return &process;
}

StThread* StNtCreateThread(
    StProcess* process,
    uint64_t entry_point,
    uint64_t user_stack,
    uint64_t kernel_stack_top
) {
    using namespace starlix::mem::user_address;

    if (!is_user_address(entry_point) ||
        !is_user_stack_pointer(user_stack) ||
        kernel_stack_top == 0ULL || (kernel_stack_top & 0xFULL) != 0ULL ||
        !is_canonical_48(kernel_stack_top - 1ULL) ||
        is_user_address(kernel_stack_top - 1ULL)) {
        return nullptr;
    }

    lock_registry();
    if (!registry_ready || !valid_process_pointer(process) ||
        process->state == PROCESS_STATE_TERMINATED ||
        process->state == PROCESS_STATE_SYSTEM ||
        process->page_directory_cr3 == 0ULL ||
        process->thread_count >= MAX_THREADS_PER_PROCESS ||
        used_threads >= MAX_THREADS || next_tid == 0ULL ||
        next_tid == ~0ULL) {
        unlock_registry();
        return nullptr;
    }

    /* A kernel stack must have one owner; sharing it would corrupt saved state. */
    for (uint32_t i = 0; i < used_threads; ++i) {
        if (thread_table[i].kernel_stack_top == kernel_stack_top &&
            thread_table[i].state != THREAD_STATE_TERMINATED) {
            unlock_registry();
            return nullptr;
        }
    }

    StThread& thread = thread_table[used_threads++];
    clear_thread(thread);
    thread.tid = next_tid++;
    thread.priority = 8u;
    thread.state = THREAD_STATE_READY;
    thread.context.rip = entry_point;
    thread.context.rsp = user_stack;
    thread.context.rflags = 0x202ULL;
    thread.context.cs = starlix::cpu::USER_CODE_SEGMENT;
    thread.context.ss = starlix::cpu::USER_DATA_SEGMENT;
    thread.kernel_stack_top = kernel_stack_top;
    thread.user_stack = user_stack;
    thread.parent_process = process;

    if (!process->main_thread) process->main_thread = &thread;
    ++process->thread_count;
    if (process->state == PROCESS_STATE_CREATED)
        process->state = PROCESS_STATE_READY;

    unlock_registry();
    return &thread;
}

bool process_abort_creation(StProcess* process, StThread* main_thread) {
    lock_registry();
    if (!registry_ready || !valid_process_pointer(process) ||
        used_processes <= 1u ||
        process != &process_table[used_processes - 1u] ||
        (process->state != PROCESS_STATE_CREATED &&
         process->state != PROCESS_STATE_READY) ||
        process->state == PROCESS_STATE_RUNNING) {
        unlock_registry();
        return false;
    }

    if (!main_thread) {
        if (process->thread_count != 0u || process->main_thread != nullptr) {
            unlock_registry();
            return false;
        }
    } else {
        if (!valid_thread_pointer(main_thread) || used_threads == 0u ||
            main_thread != &thread_table[used_threads - 1u] ||
            main_thread->parent_process != process ||
            main_thread->state != THREAD_STATE_READY ||
            process->state != PROCESS_STATE_READY ||
            process->thread_count != 1u ||
            process->main_thread != main_thread ||
            main_thread->kernel_stack_top != 0ULL ||
            main_thread->kernel_stack_base != 0ULL ||
            main_thread->kernel_stack_size != 0ULL ||
            main_thread->kernel_stack_physical_base != 0ULL ||
            main_thread->kernel_stack_allocation_id != 0u ||
            main_thread->saved_interrupt_rsp != 0ULL) {
            unlock_registry();
            return false;
        }
        clear_thread(*main_thread);
        --used_threads;
    }

    clear_process(*process);
    --used_processes;
    unlock_registry();
    return true;
}

bool process_set_state(StProcess* process, ProcessState next_state) {
    lock_registry();
    if (!registry_ready || !valid_process_pointer(process)) {
        unlock_registry();
        return false;
    }

    const ProcessState current = process->state;
    bool allowed = false;
    switch (current) {
        case PROCESS_STATE_CREATED:
            allowed = next_state == PROCESS_STATE_READY ||
                      next_state == PROCESS_STATE_TERMINATED;
            break;
        case PROCESS_STATE_READY:
            allowed = next_state == PROCESS_STATE_RUNNING ||
                      next_state == PROCESS_STATE_TERMINATED;
            break;
        case PROCESS_STATE_RUNNING:
            allowed = next_state == PROCESS_STATE_READY ||
                      next_state == PROCESS_STATE_TERMINATED;
            break;
        default:
            allowed = false;
            break;
    }
    if (allowed) {
        process->state = next_state;
        if (next_state == PROCESS_STATE_TERMINATED) {
            for (uint32_t i = 0; i < used_threads; ++i) {
                StThread& thread = thread_table[i];
                if (thread.parent_process == process &&
                    thread.state != THREAD_STATE_FREE &&
                    thread.state != THREAD_STATE_TERMINATED) {
                    thread.state = THREAD_STATE_TERMINATED;
                }
            }
        }
    }
    unlock_registry();
    return allowed;
}

bool thread_set_state(StThread* thread, ThreadState next_state) {
    lock_registry();
    if (!registry_ready || !valid_thread_pointer(thread) ||
        !valid_process_pointer(thread->parent_process) ||
        thread->parent_process->state == PROCESS_STATE_TERMINATED) {
        unlock_registry();
        return false;
    }

    const ThreadState current = thread->state;
    if (next_state == THREAD_STATE_RUNNING &&
        thread->parent_process->state != PROCESS_STATE_RUNNING) {
        unlock_registry();
        return false;
    }
    bool allowed = false;
    switch (current) {
        case THREAD_STATE_INITIALIZED:
            allowed = next_state == THREAD_STATE_READY ||
                      next_state == THREAD_STATE_TERMINATED;
            break;
        case THREAD_STATE_READY:
            allowed = next_state == THREAD_STATE_RUNNING ||
                      next_state == THREAD_STATE_TERMINATED;
            break;
        case THREAD_STATE_RUNNING:
            allowed = next_state == THREAD_STATE_READY ||
                      next_state == THREAD_STATE_BLOCKED ||
                      next_state == THREAD_STATE_TERMINATED;
            break;
        case THREAD_STATE_BLOCKED:
            allowed = next_state == THREAD_STATE_READY ||
                      next_state == THREAD_STATE_TERMINATED;
            break;
        default:
            allowed = false;
            break;
    }
    if (allowed) thread->state = next_state;
    unlock_registry();
    return allowed;
}

uint32_t process_count() {
    lock_registry();
    const uint32_t result = registry_ready ? used_processes : 0u;
    unlock_registry();
    return result;
}

uint32_t thread_count() {
    lock_registry();
    const uint32_t result = registry_ready ? used_threads : 0u;
    unlock_registry();
    return result;
}

} // namespace starlix::process
