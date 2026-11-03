// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "kernel_stack.hpp"
#include "address_space.hpp"
#include "phys.hpp"
#include "managed_page_pool.hpp"
#include "../process/process.hpp"

namespace starlix::mem {
namespace {

constexpr uint32_t RECORD_STATE_FREE = KERNEL_STACK_FREE;
constexpr uint32_t RECORD_STATE_RESERVED = KERNEL_STACK_RESERVED;
constexpr uint32_t RECORD_STATE_ALLOCATED = KERNEL_STACK_ALLOCATED;
constexpr uint32_t RECORD_STATE_BINDING = KERNEL_STACK_BINDING;
constexpr uint32_t RECORD_STATE_BOUND = KERNEL_STACK_BOUND;
constexpr uint32_t RECORD_STATE_RELEASING = KERNEL_STACK_RELEASING;

struct KernelStackRecord {
    KernelStack stack;
    uint64_t owner_tid;
    uint32_t state;
    bool mapping_removed;
};

static KernelStackRecord g_records[KERNEL_STACK_CAPACITY]{};
static uint64_t g_arena_cursor = 0ULL;
static uint32_t g_next_allocation_id = 1u;
static uint32_t g_stack_count = 0u;
static uint32_t g_lock = 0u;
static bool g_ready = false;
static KernelStackPlatform g_platform{};
static const char* g_failure_reason = "NOT_RUN";

void lock_stacks() {
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&g_lock, &expected, 1u, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        asm volatile("pause" ::: "memory");
    }
}

void unlock_stacks() {
    __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
}

uint64_t irq_save_disable() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0ULL)
        asm volatile("sti" ::: "memory");
}

bool checked_layout(uint64_t cursor, uint32_t page_count,
                    uint64_t& out_guard_base, uint64_t& out_virtual_base,
                    uint64_t& out_size, uint64_t& out_top,
                    uint64_t& out_next_cursor) {
    out_guard_base = out_virtual_base = out_size = out_top = out_next_cursor = 0ULL;
    if (page_count < KERNEL_STACK_MIN_PAGES ||
        page_count > KERNEL_STACK_MAX_PAGES ||
        (cursor & (KERNEL_STACK_PAGE_BYTES - 1ULL)) != 0ULL) return false;

    const uint64_t span_pages = static_cast<uint64_t>(page_count) + 2ULL;
    if (span_pages > (~0ULL / KERNEL_STACK_PAGE_BYTES)) return false;
    const uint64_t span_bytes = span_pages * KERNEL_STACK_PAGE_BYTES;
    if (cursor > (KERNEL_STACK_REGION_END - KERNEL_STACK_REGION_BASE) ||
        span_bytes > (KERNEL_STACK_REGION_END - KERNEL_STACK_REGION_BASE) - cursor)
        return false;

    out_guard_base = KERNEL_STACK_REGION_BASE + cursor;
    out_virtual_base = out_guard_base + KERNEL_STACK_PAGE_BYTES;
    out_size = static_cast<uint64_t>(page_count) * KERNEL_STACK_PAGE_BYTES;
    out_top = out_virtual_base + out_size;
    out_next_cursor = cursor + span_bytes;
    return out_guard_base >= KERNEL_STACK_REGION_BASE &&
           out_top + KERNEL_STACK_PAGE_BYTES <= KERNEL_STACK_REGION_END &&
           ((out_virtual_base >> 39) & 0x1FFULL) == KERNEL_STACK_PML4_INDEX;
}

bool record_matches(const KernelStackRecord& record, const KernelStack& stack) {
    return record.state != RECORD_STATE_FREE &&
           record.stack.magic == KERNEL_STACK_MAGIC &&
           stack.magic == KERNEL_STACK_MAGIC &&
           record.stack.allocation_id == stack.allocation_id &&
           record.stack.virtual_base == stack.virtual_base &&
           record.stack.physical_base == stack.physical_base &&
           record.stack.size == stack.size &&
           record.stack.top == stack.top &&
           record.stack.page_count == stack.page_count;
}

int find_record(const KernelStack& stack) {
    for (uint32_t i = 0; i < KERNEL_STACK_CAPACITY; ++i)
        if (record_matches(g_records[i], stack)) return static_cast<int>(i);
    return -1;
}

} // namespace

bool kernel_stack_manager_init(const KernelStackPlatform& platform) {
    if (!platform.critical_enter || !platform.critical_leave ||
        kernel_stack_manager_ready() ||
        !phys_allocator_ready() || !managed_page_pool_ready() ||
        !address_space_manager_ready() ||
        address_space_kernel_root() == 0ULL) return false;

    const uint64_t saved_flags = platform.critical_enter();
    lock_stacks();
    for (uint32_t i = 0; i < KERNEL_STACK_CAPACITY; ++i)
        g_records[i] = KernelStackRecord{};
    g_arena_cursor = 0ULL;
    g_next_allocation_id = 1u;
    g_stack_count = 0u;
    g_platform = platform;
    g_ready = true;
    unlock_stacks();
    platform.critical_leave(saved_flags);
    return true;
}

bool kernel_stack_init() {
    KernelStackPlatform platform{};
    platform.critical_enter = irq_save_disable;
    platform.critical_leave = irq_restore;
    return kernel_stack_manager_init(platform);
}

bool kernel_stack_manager_ready() {
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE);
}

const char* kernel_stack_failure_reason() { return g_failure_reason; }

bool kernel_stack_allocate(uint32_t page_count, KernelStack& out_stack) {
    out_stack = KernelStack{};
    if (!kernel_stack_manager_ready() || !phys_allocator_ready()) return false;

    const uint64_t saved_flags = g_platform.critical_enter();
    lock_stacks();
    uint32_t slot = KERNEL_STACK_CAPACITY;
    for (uint32_t i = 0; i < KERNEL_STACK_CAPACITY; ++i) {
        if (g_records[i].state == RECORD_STATE_FREE) {
            slot = i;
            break;
        }
    }
    uint64_t guard_base = 0ULL, virtual_base = 0ULL, size = 0ULL;
    uint64_t top = 0ULL, next_cursor = 0ULL;
    if (slot == KERNEL_STACK_CAPACITY || g_next_allocation_id == 0u ||
        !checked_layout(g_arena_cursor, page_count, guard_base,
                        virtual_base, size, top, next_cursor)) {
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }

    KernelStack reserved{};
    (void)guard_base;
    reserved.virtual_base = virtual_base;
    reserved.size = size;
    reserved.top = top;
    reserved.page_count = page_count;
    reserved.allocation_id = g_next_allocation_id++;
    reserved.magic = KERNEL_STACK_MAGIC;
    g_records[slot].stack = reserved;
    g_records[slot].owner_tid = 0ULL;
    g_records[slot].state = RECORD_STATE_RESERVED;
    g_arena_cursor = next_cursor; // Monotonic: never recycle a guard/stack VA.
    unlock_stacks();

    const uint64_t physical = managed_alloc_phys_pages(page_count);
    if (physical == 0ULL || !user_address::page_aligned(physical) ||
        !address_space_map_kernel_stack_range(virtual_base, physical, page_count)) {
        if (physical != 0ULL)
            (void)managed_release_phys_pages(physical, page_count);
        lock_stacks();
        g_records[slot] = KernelStackRecord{};
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false; // Guard/VA reservations remain monotonic by design.
    }

    lock_stacks();
    if (g_records[slot].state != RECORD_STATE_RESERVED ||
        g_records[slot].stack.allocation_id != reserved.allocation_id) {
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }
    g_records[slot].stack.physical_base = physical;
    g_records[slot].state = RECORD_STATE_ALLOCATED;
    ++g_stack_count;
    out_stack = g_records[slot].stack;
    unlock_stacks();
    g_platform.critical_leave(saved_flags);
    return true;
}

bool kernel_stack_allocate_default(KernelStack& out_stack) {
    return kernel_stack_allocate(KERNEL_STACK_DEFAULT_PAGES, out_stack);
}

bool kernel_stack_release(KernelStack& stack,
                          starlix::process::StThread* owner_thread) {
    using namespace starlix::process;
    if (!kernel_stack_manager_ready() || stack.magic != KERNEL_STACK_MAGIC)
        return false;
    if (owner_thread && (!thread_is_registered(owner_thread) ||
        owner_thread->state != THREAD_STATE_READY ||
        !owner_thread->parent_process ||
        !process_is_registered(owner_thread->parent_process) ||
        owner_thread->parent_process->state == PROCESS_STATE_RUNNING ||
        owner_thread->parent_process->state == PROCESS_STATE_TERMINATED ||
        owner_thread->kernel_stack_top != stack.top)) return false;

    const uint64_t saved_flags = g_platform.critical_enter();
    lock_stacks();
    const int slot = find_record(stack);
    if (slot < 0) {
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }

    KernelStackRecord& record = g_records[static_cast<uint32_t>(slot)];
    const uint32_t prior_state = record.state;
    const bool was_bound = prior_state == RECORD_STATE_BOUND ||
        (prior_state == RECORD_STATE_RELEASING && record.owner_tid != 0ULL);
    if ((prior_state != RECORD_STATE_ALLOCATED &&
         prior_state != RECORD_STATE_BOUND &&
         prior_state != RECORD_STATE_RELEASING) ||
        (was_bound && (!owner_thread || record.owner_tid != owner_thread->tid)) ||
        (!was_bound && record.owner_tid != 0ULL) ||
        (owner_thread && !was_bound &&
         (owner_thread->kernel_stack_base != 0ULL ||
          owner_thread->kernel_stack_size != 0ULL ||
          owner_thread->saved_interrupt_rsp != 0ULL ||
          owner_thread->kernel_stack_allocation_id != 0u ||
          owner_thread->kernel_stack_physical_base != 0ULL))) {
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }
    record.state = RECORD_STATE_RELEASING;
    unlock_stacks();

    bool mapping_removed = false;
    lock_stacks();
    mapping_removed = g_records[static_cast<uint32_t>(slot)].mapping_removed;
    unlock_stacks();
    if (!mapping_removed) {
        if (!address_space_unmap_kernel_stack_range(
                stack.virtual_base, stack.physical_base, stack.page_count)) {
            lock_stacks();
            if (g_records[static_cast<uint32_t>(slot)].state == RECORD_STATE_RELEASING)
                g_records[static_cast<uint32_t>(slot)].state = prior_state;
            unlock_stacks();
            g_platform.critical_leave(saved_flags);
            return false;
        }
        lock_stacks();
        if (g_records[static_cast<uint32_t>(slot)].state != RECORD_STATE_RELEASING) {
            unlock_stacks();
            g_platform.critical_leave(saved_flags);
            return false;
        }
        g_records[static_cast<uint32_t>(slot)].mapping_removed = true;
        unlock_stacks();
    }

    if (!managed_release_phys_pages(stack.physical_base, stack.page_count)) {
        g_platform.critical_leave(saved_flags);
        return false; // Keep the RELEASING record so cleanup can be retried.
    }

    if (owner_thread) {
        owner_thread->kernel_stack_base = 0ULL;
        owner_thread->kernel_stack_size = 0ULL;
        owner_thread->kernel_stack_top = 0ULL;
        owner_thread->kernel_stack_physical_base = 0ULL;
        owner_thread->kernel_stack_allocation_id = 0u;
        owner_thread->saved_interrupt_rsp = 0ULL;
    }

    lock_stacks();
    KernelStackRecord& finished = g_records[static_cast<uint32_t>(slot)];
    const bool still_owned = finished.state == RECORD_STATE_RELEASING &&
        record_matches(finished, stack) &&
        finished.mapping_removed;
    if (still_owned) {
        finished = KernelStackRecord{};
        if (g_stack_count != 0u) --g_stack_count;
    }
    unlock_stacks();
    g_platform.critical_leave(saved_flags);
    if (!still_owned) return false;
    stack = KernelStack{};
    return true;
}

bool kernel_stack_bind_thread(const KernelStack& stack,
                              starlix::process::StThread* thread) {
    using namespace starlix::process;
    g_failure_reason = "BIND_PRECONDITION";
    if (!kernel_stack_manager_ready()) {
        g_failure_reason = "ARENA_UNAVAILABLE";
        return false;
    }
    if (!thread || !thread_is_registered(thread)) {
        g_failure_reason = "THREAD_NOT_REGISTERED";
        return false;
    }
    if (!thread->parent_process || !process_is_registered(thread->parent_process)) {
        g_failure_reason = "PARENT_PROCESS_NOT_REGISTERED";
        return false;
    }

    const uint64_t saved_flags = g_platform.critical_enter();
    lock_stacks();
    const int slot = find_record(stack);
    if (slot < 0 || g_records[static_cast<uint32_t>(slot)].state !=
                        RECORD_STATE_ALLOCATED ||
        thread->kernel_stack_top != stack.top ||
        thread->kernel_stack_base != 0ULL ||
        thread->kernel_stack_size != 0ULL ||
        thread->saved_interrupt_rsp != 0ULL ||
        thread->kernel_stack_allocation_id != 0u ||
        thread->kernel_stack_physical_base != 0ULL ||
        thread->state != THREAD_STATE_READY ||
        thread->parent_process->page_directory_cr3 == 0ULL) {
        g_failure_reason = "STACK_RECORD_OR_THREAD_STATE_MISMATCH";
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }
    g_records[static_cast<uint32_t>(slot)].state = RECORD_STATE_BINDING;
    unlock_stacks();

    const uint64_t root = thread->parent_process->page_directory_cr3;
    if (!address_space_kernel_stack_range_mapped(
            root, stack.virtual_base, stack.physical_base, stack.page_count)) {
        g_failure_reason = address_space_kernel_stack_mapping_reason();
        lock_stacks();
        if (g_records[static_cast<uint32_t>(slot)].state == RECORD_STATE_BINDING)
            g_records[static_cast<uint32_t>(slot)].state = RECORD_STATE_ALLOCATED;
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }
    if (!address_space_kernel_stack_guard_unmapped(
            root, stack.virtual_base - KERNEL_STACK_PAGE_BYTES) ||
        !address_space_kernel_stack_guard_unmapped(root, stack.top)) {
        g_failure_reason = "STACK_GUARD_PAGE_MAPPED";
        lock_stacks();
        if (g_records[static_cast<uint32_t>(slot)].state == RECORD_STATE_BINDING)
            g_records[static_cast<uint32_t>(slot)].state = RECORD_STATE_ALLOCATED;
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }
    if (!thread_prepare_user_interrupt_frame(
            thread, stack.virtual_base, stack.size)) {
        g_failure_reason = "INITIAL_USER_IRETQ_FRAME_REJECTED";
        lock_stacks();
        if (g_records[static_cast<uint32_t>(slot)].state == RECORD_STATE_BINDING)
            g_records[static_cast<uint32_t>(slot)].state = RECORD_STATE_ALLOCATED;
        unlock_stacks();
        g_platform.critical_leave(saved_flags);
        return false;
    }

    thread->kernel_stack_physical_base = stack.physical_base;
    thread->kernel_stack_allocation_id = stack.allocation_id;
    lock_stacks();
    KernelStackRecord& record = g_records[static_cast<uint32_t>(slot)];
    const bool commit = record.state == RECORD_STATE_BINDING &&
                        record_matches(record, stack) && record.owner_tid == 0ULL;
    if (commit) {
        record.owner_tid = thread->tid;
        record.state = RECORD_STATE_BOUND;
    } else {
        g_failure_reason = "STACK_OWNER_COMMIT_FAILED";
        thread->kernel_stack_physical_base = 0ULL;
        thread->kernel_stack_allocation_id = 0u;
    }
    unlock_stacks();
    g_platform.critical_leave(saved_flags);
    if (commit) g_failure_reason = "PASS";
    return commit;
}

bool kernel_stack_thread_valid(const starlix::process::StThread* thread,
                               uint64_t process_root) {
    using namespace starlix::process;
    if (!kernel_stack_manager_ready() || !thread ||
        !thread_is_registered(thread) || thread->tid == 0ULL ||
        thread->kernel_stack_allocation_id == 0u ||
        thread->kernel_stack_physical_base == 0ULL ||
        thread->kernel_stack_base == 0ULL || thread->kernel_stack_size == 0ULL ||
        thread->kernel_stack_top != thread->kernel_stack_base + thread->kernel_stack_size ||
        !thread->parent_process ||
        !process_is_registered(thread->parent_process) ||
        thread->parent_process->state == PROCESS_STATE_SYSTEM ||
        thread->parent_process->state == PROCESS_STATE_TERMINATED ||
        thread->parent_process->page_directory_cr3 != process_root) return false;

    const uint64_t saved_flags = g_platform.critical_enter();
    lock_stacks();
    const KernelStackRecord* record = nullptr;
    for (uint32_t i = 0; i < KERNEL_STACK_CAPACITY; ++i) {
        const KernelStackRecord& candidate = g_records[i];
        if (candidate.state == RECORD_STATE_BOUND &&
            candidate.owner_tid == thread->tid &&
            candidate.stack.allocation_id == thread->kernel_stack_allocation_id) {
            record = &candidate;
            break;
        }
    }
    KernelStack copy{};
    if (record) copy = record->stack;
    unlock_stacks();

    const bool metadata = record && copy.magic == KERNEL_STACK_MAGIC &&
        copy.virtual_base == thread->kernel_stack_base &&
        copy.physical_base == thread->kernel_stack_physical_base &&
        copy.size == thread->kernel_stack_size &&
        copy.top == thread->kernel_stack_top;
    const bool mapped = metadata && address_space_kernel_stack_range_mapped(
        process_root, copy.virtual_base, copy.physical_base, copy.page_count);
    const bool guards = mapped &&
        address_space_kernel_stack_guard_unmapped(
            process_root, copy.virtual_base - KERNEL_STACK_PAGE_BYTES) &&
        address_space_kernel_stack_guard_unmapped(process_root, copy.top);
    g_platform.critical_leave(saved_flags);
    return guards;
}

uint32_t kernel_stack_count() {
    const uint64_t saved_flags = g_platform.critical_enter
        ? g_platform.critical_enter() : 0ULL;
    lock_stacks();
    const uint32_t result = g_stack_count;
    unlock_stacks();
    if (g_platform.critical_leave) g_platform.critical_leave(saved_flags);
    return result;
}

} // namespace starlix::mem
