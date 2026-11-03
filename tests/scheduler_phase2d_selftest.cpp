#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../kernel/src/mem/user_address.hpp"
#include "../kernel/src/process/process.hpp"
#include "../kernel/src/process/scheduler.hpp"

using namespace starlix::mem::user_address;
using namespace starlix::process;
using namespace starlix::process::scheduler;

static unsigned checks = 0;
static unsigned failures = 0;

static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("[FAIL] %s\n", name);
    } else {
        std::printf("[PASS] %s\n", name);
    }
}

static StThread* make_thread(const char* name, uint64_t cr3,
                             uint64_t kernel_stack, uint32_t priority) {
    StProcess* process = StNtCreateProcess(name, cr3);
    if (!process) return nullptr;
    StThread* thread = StNtCreateThread(
        process, USER_IMAGE_BASE, USER_STACK_TOP, kernel_stack);
    if (!thread) return nullptr;
    thread->priority = priority;
    return thread;
}

int main() {
    process_init();
    scheduler_init();
    check(scheduler_ready(), "scheduler publishes ready state after initialization");
    check(scheduler_ready_count() == 0u && scheduler_pick_next() == nullptr,
          "empty ready set has no runnable candidate");

    StThread* low = make_thread("low.ect", 0x00400000ULL,
                                0x01000000ULL, 3u);
    StThread* high_a = make_thread("high-a.ect", 0x00500000ULL,
                                   0x01010000ULL, 12u);
    StThread* high_b = make_thread("high-b.ect", 0x00600000ULL,
                                   0x01020000ULL, 12u);
    StThread* highest = make_thread("highest.ect", 0x00700000ULL,
                                    0x01030000ULL, 31u);
    check(low && high_a && high_b && highest,
          "test processes and independent READY threads are created");
    check(low && thread_is_registered(low) &&
          !thread_is_registered(reinterpret_cast<StThread*>(0x1234ULL)),
          "thread registry validates object identity before scheduler admission");

    StThread foreign{};
    check(!scheduler_admit(nullptr) && !scheduler_admit(&foreign),
          "null and foreign thread pointers are rejected");
    check(low && scheduler_admit(low),
          "registered READY thread is admitted");
    check(low && scheduler_is_admitted(low),
          "scheduler confirms exact READY thread admission by identity");
    check(high_a && !scheduler_is_admitted(high_a),
          "scheduler rejects a registered thread that has not been admitted");
    check(low && !scheduler_admit(low),
          "duplicate admission is rejected");
    check(high_a && scheduler_admit(high_a) &&
          high_b && scheduler_admit(high_b) &&
          highest && scheduler_admit(highest),
          "multiple priority groups are admitted");
    check(scheduler_ready_count() == 4u,
          "ready count reports admitted runnable threads");
    check(scheduler_pick_next() == highest,
          "highest priority thread is selected first");
    check(scheduler_remove(highest->tid),
          "highest-priority thread can be removed for lower-group policy checks");
    check(highest && !scheduler_is_admitted(highest),
          "scheduler admission query clears after ready-set removal");
    check(scheduler_pick_next() == high_a,
          "equal-priority group starts in FIFO order");
    check(scheduler_pick_next() == high_b,
          "equal-priority group rotates to its next thread");
    check(scheduler_pick_next() == high_a,
          "equal-priority round-robin returns to its first thread");

    /* A selected thread remains READY until context switching is implemented;
       policy selection must not mutate process/thread lifecycle state. */
    check(highest->state == THREAD_STATE_READY &&
          highest->parent_process->state == PROCESS_STATE_READY,
          "selection does not pretend to perform a context switch");

    check(scheduler_set_priority(low, 20u),
          "registered queued thread priority can be changed");
    check(scheduler_pick_next() == low,
          "priority update affects the next scheduling decision");
    check(!scheduler_set_priority(low, 32u),
          "out-of-range priority is rejected");

    check(scheduler_remove(low->tid) &&
          !scheduler_remove(low->tid),
          "ready thread can be removed exactly once");
    check(scheduler_ready_count() == 2u,
          "remove updates ready-set accounting");

    StThread* blocked = make_thread("blocked.ect", 0x00800000ULL,
                                    0x01040000ULL, 30u);
    check(blocked && scheduler_admit(blocked),
          "additional high-priority thread can be admitted");
    check(blocked && process_set_state(blocked->parent_process,
                                      PROCESS_STATE_RUNNING) &&
          thread_set_state(blocked, THREAD_STATE_RUNNING) &&
          thread_set_state(blocked, THREAD_STATE_BLOCKED),
          "running thread can enter blocked state before selection");
    check(scheduler_pick_next() == high_b &&
          scheduler_ready_count() == 2u,
          "blocked thread is skipped and pruned from ready set");
    check(!scheduler_admit(blocked),
          "blocked thread cannot be re-admitted before becoming READY");
    check(blocked && thread_set_state(blocked, THREAD_STATE_READY) &&
          scheduler_admit(blocked),
          "unblocked READY thread can be admitted again");

    StThread* doomed = make_thread("doomed.ect", 0x00900000ULL,
                                   0x01050000ULL, 29u);
    check(doomed && scheduler_admit(doomed),
          "thread owned by a live process can be admitted");
    check(doomed && process_set_state(doomed->parent_process,
                                      PROCESS_STATE_TERMINATED),
          "owner process can terminate");
    check(scheduler_pick_next() != doomed,
          "terminated process thread is never selected");

    StThread* invalid_priority = make_thread("invalid-priority.ect",
        0x00A00000ULL, 0x01060000ULL, 32u);
    check(invalid_priority && !scheduler_admit(invalid_priority),
          "thread priority above configured maximum is rejected");

    process_init();
    scheduler_init();
    bool capacity_ok = true;
    StThread* capacity_threads[MAX_THREADS]{};
    uint32_t capacity_thread_count = 0u;
    for (uint32_t p = 0; p < 8u && capacity_ok; ++p) {
        char name[32] = {'c','a','p','-','0','0','0','\0'};
        name[4] = static_cast<char>('0' + p);
        StProcess* owner = StNtCreateProcess(
            name, 0x02000000ULL + static_cast<uint64_t>(p) * 0x10000ULL);
        if (!owner) { capacity_ok = false; break; }
        for (uint32_t t = 0; t < MAX_THREADS_PER_PROCESS; ++t) {
            const uint64_t stack_top = 0x03000000ULL +
                (static_cast<uint64_t>(p) * MAX_THREADS_PER_PROCESS + t) * 0x10000ULL;
            StThread* thread = StNtCreateThread(
                owner, USER_IMAGE_BASE, USER_STACK_TOP, stack_top);
            if (!thread || capacity_thread_count >= MAX_THREADS) {
                capacity_ok = false;
                break;
            }
            capacity_threads[capacity_thread_count++] = thread;
        }
    }
    check(capacity_ok && capacity_thread_count == MAX_THREADS,
          "process registry creates enough threads to test scheduler capacity");
    bool admitted_all = true;
    for (uint32_t i = 0; i < MAX_READY_THREADS; ++i) {
        if (!scheduler_admit(capacity_threads[i])) {
            admitted_all = false;
            break;
        }
    }
    check(admitted_all && scheduler_ready_count() == MAX_READY_THREADS,
          "bounded scheduler ready set reaches its independent capacity");
    check(!scheduler_admit(capacity_threads[MAX_READY_THREADS]),
          "scheduler refuses ready-set overflow without corrupting entries");
    check(scheduler_ready_count() == MAX_READY_THREADS,
          "overflow rejection preserves ready-set accounting");

    std::printf("SCHEDULER_PHASE2D_SELFTEST: %s (%u checks, %u failures)\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
