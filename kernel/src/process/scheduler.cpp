// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "scheduler.hpp"

namespace starlix::process::scheduler {
namespace {

struct ReadyEntry {
    StThread* thread;
    uint64_t tid;
    uint64_t order;
    bool active;
};

static ReadyEntry ready_set[MAX_READY_THREADS]{};
static uint64_t next_order = 1ULL;
static uint32_t ready_entries = 0u;
static uint32_t scheduler_lock = 0u;
static bool initialized = false;

static void lock_scheduler() {
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&scheduler_lock, &expected, 1u, false,
                                        __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        asm volatile("pause" ::: "memory");
    }
}

static void unlock_scheduler() {
    __atomic_store_n(&scheduler_lock, 0u, __ATOMIC_RELEASE);
}

static bool thread_is_runnable(const ReadyEntry& entry) {
    if (!entry.active || !entry.thread || entry.tid == 0ULL) return false;
    const StThread* thread = entry.thread;
    if (thread->tid != entry.tid ||
        thread->state != THREAD_STATE_READY ||
        thread->priority > MAX_PRIORITY ||
        !thread->parent_process) {
        return false;
    }
    const ProcessState process_state = thread->parent_process->state;
    return process_state == PROCESS_STATE_READY ||
           process_state == PROCESS_STATE_RUNNING;
}

static int find_free_slot() {
    for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
        if (!ready_set[i].active) return static_cast<int>(i);
    }
    return -1;
}

static int find_tid(uint64_t tid) {
    if (tid == 0ULL) return -1;
    for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
        if (ready_set[i].active && ready_set[i].tid == tid)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

void scheduler_init() {
    lock_scheduler();
    for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
        ready_set[i].thread = nullptr;
        ready_set[i].tid = 0ULL;
        ready_set[i].order = 0ULL;
        ready_set[i].active = false;
    }
    next_order = 1ULL;
    ready_entries = 0u;
    initialized = true;
    unlock_scheduler();
}

bool scheduler_ready() {
    return __atomic_load_n(&initialized, __ATOMIC_ACQUIRE);
}

bool scheduler_admit(StThread* thread) {
    /* Validate pointer identity before dereferencing caller-provided memory. */
    if (!thread_is_registered(thread)) return false;
    if (thread->state != THREAD_STATE_READY || !thread->parent_process ||
        thread->priority > MAX_PRIORITY) {
        return false;
    }
    const ProcessState owner_state = thread->parent_process->state;
    if (owner_state != PROCESS_STATE_READY &&
        owner_state != PROCESS_STATE_RUNNING) {
        return false;
    }

    lock_scheduler();
    if (!initialized || ready_entries >= MAX_READY_THREADS ||
        next_order == 0ULL || next_order == ~0ULL ||
        find_tid(thread->tid) >= 0) {
        unlock_scheduler();
        return false;
    }
    const int slot = find_free_slot();
    if (slot < 0) {
        unlock_scheduler();
        return false;
    }

    ReadyEntry& entry = ready_set[static_cast<uint32_t>(slot)];
    entry.thread = thread;
    entry.tid = thread->tid;
    entry.order = next_order++;
    entry.active = true;
    ++ready_entries;
    unlock_scheduler();
    return true;
}

bool scheduler_remove(uint64_t tid) {
    lock_scheduler();
    const int slot = initialized ? find_tid(tid) : -1;
    if (slot < 0) {
        unlock_scheduler();
        return false;
    }
    ReadyEntry& entry = ready_set[static_cast<uint32_t>(slot)];
    entry.thread = nullptr;
    entry.tid = 0ULL;
    entry.order = 0ULL;
    entry.active = false;
    --ready_entries;
    unlock_scheduler();
    return true;
}

bool scheduler_is_admitted(const StThread* thread) {
    if (!thread_is_registered(thread)) return false;
    lock_scheduler();
    const int slot = initialized ? find_tid(thread->tid) : -1;
    const bool result = slot >= 0 &&
        ready_set[static_cast<uint32_t>(slot)].thread == thread &&
        thread_is_runnable(ready_set[static_cast<uint32_t>(slot)]);
    unlock_scheduler();
    return result;
}

bool scheduler_set_priority(StThread* thread, uint32_t priority) {
    if (priority > MAX_PRIORITY || !thread_is_registered(thread))
        return false;
    lock_scheduler();
    const int slot = initialized ? find_tid(thread->tid) : -1;
    if (slot < 0 || ready_set[static_cast<uint32_t>(slot)].thread != thread) {
        unlock_scheduler();
        return false;
    }
    if (next_order == 0ULL || next_order == ~0ULL) {
        unlock_scheduler();
        return false;
    }
    thread->priority = priority;
    ready_set[static_cast<uint32_t>(slot)].order = next_order++;
    unlock_scheduler();
    return true;
}

StThread* scheduler_pick_next() {
    lock_scheduler();
    if (!initialized || ready_entries == 0u ||
        next_order == 0ULL || next_order == ~0ULL) {
        unlock_scheduler();
        return nullptr;
    }

    int selected = -1;
    uint32_t selected_priority = 0u;
    uint64_t selected_order = ~0ULL;

    /* Remove entries whose thread/process became blocked or terminated. */
    for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
        ReadyEntry& entry = ready_set[i];
        if (!entry.active) continue;
        if (!thread_is_runnable(entry)) {
            entry.thread = nullptr;
            entry.tid = 0ULL;
            entry.order = 0ULL;
            entry.active = false;
            --ready_entries;
            continue;
        }

        const uint32_t priority = entry.thread->priority;
        if (selected < 0 || priority > selected_priority ||
            (priority == selected_priority && entry.order < selected_order)) {
            selected = static_cast<int>(i);
            selected_priority = priority;
            selected_order = entry.order;
        }
    }

    if (selected < 0) {
        unlock_scheduler();
        return nullptr;
    }

    ReadyEntry& chosen = ready_set[static_cast<uint32_t>(selected)];
    StThread* result = chosen.thread;
    /* Move to the tail of its priority group for round-robin fairness. */
    chosen.order = next_order++;
    unlock_scheduler();
    return result;
}

uint32_t scheduler_ready_count() {
    lock_scheduler();
    uint32_t result = 0u;
    if (initialized) {
        for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
            if (thread_is_runnable(ready_set[i])) ++result;
        }
    }
    unlock_scheduler();
    return result;
}

} // namespace starlix::process::scheduler
