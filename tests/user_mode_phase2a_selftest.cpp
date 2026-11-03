#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../kernel/src/mem/user_address.hpp"
#include "../kernel/src/process/process.hpp"

using namespace starlix::mem::user_address;
using namespace starlix::process;

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

int main() {
    check(((USER_REGION_BASE >> 39) & 0x1FFULL) == 1ULL,
          "reserved user region occupies PML4 slot 1");
    check(is_canonical_48(USER_REGION_BASE),
          "user region base is canonical 48-bit address");
    check(is_user_address(USER_IMAGE_BASE),
          "planned application image address is in user region");
    check(is_user_address(USER_STACK_BASE),
          "planned user stack base is in user region");
    check(!is_user_address(USER_REGION_END),
          "user region end is exclusive");
    check(!is_user_address(0x0000000000200000ULL),
          "low identity-mapped kernel address is outside user region");
    check(!is_user_address(0xFFFF800000000000ULL),
          "canonical kernel-half address is rejected as user address");
    check(!is_user_range(USER_REGION_END - 8ULL, 16ULL),
          "user range crossing region end is rejected");
    check(!is_user_range(USER_REGION_BASE, 0ULL),
          "zero-length user range is rejected");
    check(!is_user_range(USER_REGION_BASE, ~0ULL),
          "overflowing user range is rejected");

    uint64_t page_base = 0;
    uint64_t page_count = 0;
    check(checked_page_span(USER_IMAGE_BASE + 17ULL, 5000ULL,
                            page_base, page_count) &&
          page_base == USER_IMAGE_BASE && page_count == 2ULL,
          "unaligned user buffer rounds to exact two-page span");
    check(!checked_page_span(USER_REGION_END - 8ULL, 16ULL,
                             page_base, page_count),
          "page-span helper rejects range outside user reservation");

    uint64_t pte = 0;
    constexpr uint64_t physical = 0x0000000123400000ULL;
    check(build_user_pte(physical,
                         USER_PAGE_READ | USER_PAGE_EXEC,
                         true, pte) &&
          (pte & (PTE_PRESENT | PTE_USER)) == (PTE_PRESENT | PTE_USER) &&
          (pte & PTE_WRITABLE) == 0 && (pte & PTE_NX) == 0,
          "read/execute PTE is user-accessible and non-writable");
    check(build_user_pte(physical,
                         USER_PAGE_READ | USER_PAGE_WRITE,
                         true, pte) &&
          (pte & PTE_WRITABLE) != 0 && (pte & PTE_NX) != 0,
          "read/write PTE is non-executable");
    check(build_user_pte(physical, USER_PAGE_READ, true, pte) &&
          (pte & PTE_NX) != 0,
          "read-only PTE is non-executable");
    check(!build_user_pte(physical,
                          USER_PAGE_READ | USER_PAGE_WRITE | USER_PAGE_EXEC,
                          true, pte),
          "W^X policy rejects writable executable mapping");
    check(!build_user_pte(physical, USER_PAGE_WRITE, true, pte),
          "mapping without read permission is rejected");
    check(!build_user_pte(physical, USER_PAGE_READ, false, pte),
          "non-executable page is rejected when NX is unavailable");
    check(!build_user_pte(physical + 1ULL, USER_PAGE_READ, true, pte),
          "unaligned physical page is rejected");
    check(!build_user_pte(0x0010000000000000ULL,
                          USER_PAGE_READ, true, pte),
          "physical address outside PTE address field is rejected");
    check(!build_user_pte(physical, 0x80000000u, true, pte),
          "unknown permission bits are rejected");

    process_init();
    StProcess* system = process_system();
    check(system && system->pid == SYSTEM_PROCESS_ID &&
          system->state == PROCESS_STATE_SYSTEM &&
          std::strcmp(system->image_name, "System") == 0,
          "system process is registered as PID 4");
    check(process_count() == 1u && thread_count() == 0u,
          "registry counters start with only the system process");

    StProcess* alpha = StNtCreateProcess("smgrsb.ect", 0x00400000ULL);
    StProcess* beta = StNtCreateProcess("stlgn.ect", 0x00500000ULL);
    check(alpha && beta && alpha != beta && alpha->pid != beta->pid,
          "two process creations receive distinct persistent objects and PIDs");
    check(alpha && alpha->page_directory_cr3 == 0x00400000ULL &&
          std::strcmp(alpha->image_name, "smgrsb.ect") == 0,
          "process metadata retains CR3 and image name");
    check(alpha && process_find(alpha->pid) == alpha,
          "process lookup returns the registered process object");
    check(!StNtCreateProcess(nullptr, 0x00600000ULL),
          "null process image name is rejected");
    check(!StNtCreateProcess("bad-cr3", 0ULL),
          "zero user CR3 is rejected");
    check(!StNtCreateProcess("bad-align", 0x00400001ULL),
          "unaligned user CR3 is rejected");
    char too_long_name[33];
    std::memset(too_long_name, 'x', 32);
    too_long_name[32] = '\0';
    check(!StNtCreateProcess(too_long_name, 0x00600000ULL),
          "unterminated 32-byte process image name is rejected");

    StThread* alpha_main = StNtCreateThread(
        alpha, USER_IMAGE_BASE, USER_STACK_TOP, 0x00800000ULL);
    StThread* alpha_worker = StNtCreateThread(
        alpha, USER_IMAGE_BASE + 0x1000ULL,
        USER_STACK_TOP - 0x1000ULL, 0x00810000ULL);
    check(alpha_main && alpha_worker && alpha_main != alpha_worker &&
          alpha_main->tid != alpha_worker->tid,
          "multiple threads receive distinct objects and TIDs");
    check(alpha && alpha->main_thread == alpha_main &&
          alpha->thread_count == 2u &&
          alpha->state == PROCESS_STATE_READY,
          "process tracks its main thread and thread count");
    check(alpha_main && alpha_main->context.rip == USER_IMAGE_BASE &&
          alpha_main->context.rsp == USER_STACK_TOP &&
          alpha_main->context.cs == 0x23ULL &&
          alpha_main->context.ss == 0x1BULL &&
          alpha_main->context.rflags == 0x202ULL,
          "thread initial context uses user selectors and safe RFLAGS");
    check(alpha_main && alpha_main->kernel_stack_top == 0x00800000ULL,
          "thread retains its dedicated kernel stack top");
    check(alpha_main && thread_find(alpha_main->tid) == alpha_main,
          "thread lookup returns the registered thread object");
    check(!thread_set_state(alpha_main, THREAD_STATE_RUNNING),
          "thread cannot run while its owning process is not running");

    check(!StNtCreateThread(alpha, 0x0000000000200000ULL,
                            USER_STACK_TOP, 0x00820000ULL),
          "kernel identity address cannot be a user entry point");
    check(!StNtCreateThread(alpha, USER_IMAGE_BASE,
                            USER_REGION_END + 0x10ULL, 0x00820000ULL),
          "out-of-region user stack is rejected");
    check(!StNtCreateThread(alpha, USER_IMAGE_BASE,
                            USER_STACK_TOP, 0x00820001ULL),
          "misaligned kernel stack top is rejected");
    check(!StNtCreateThread(alpha, USER_IMAGE_BASE,
                            USER_STACK_TOP, 0x00800000ULL),
          "kernel stack cannot be shared by two live threads");
    check(!StNtCreateThread(alpha, USER_IMAGE_BASE,
                            USER_STACK_TOP, USER_STACK_TOP),
          "user virtual address cannot be used as a kernel stack");
    StProcess foreign{};
    check(!StNtCreateThread(&foreign, USER_IMAGE_BASE,
                            USER_STACK_TOP, 0x00820000ULL),
          "unregistered process pointer is rejected");

    check(process_set_state(alpha, PROCESS_STATE_RUNNING),
          "ready process can transition to running");
    check(!process_set_state(alpha, PROCESS_STATE_CREATED),
          "invalid process state transition is rejected");
    check(thread_set_state(alpha_main, THREAD_STATE_RUNNING),
          "ready main thread can transition to running");
    check(thread_set_state(alpha_main, THREAD_STATE_BLOCKED),
          "running thread can transition to blocked");
    check(thread_set_state(alpha_main, THREAD_STATE_READY),
          "blocked thread can transition to ready");
    check(thread_set_state(alpha_main, THREAD_STATE_TERMINATED),
          "ready thread can transition to terminated");
    check(!thread_set_state(alpha_main, THREAD_STATE_RUNNING),
          "terminated thread cannot be restarted");
    check(process_set_state(alpha, PROCESS_STATE_TERMINATED),
          "running process can transition to terminated");
    check(alpha_worker->state == THREAD_STATE_TERMINATED,
          "process termination also terminates its remaining threads");
    check(!thread_set_state(alpha_worker, THREAD_STATE_READY),
          "thread in a terminated process cannot be restarted");
    check(!process_set_state(alpha, PROCESS_STATE_READY),
          "terminated process cannot be restarted");

    process_init();
    bool capacity_ok = true;
    for (uint32_t i = 0; i < (MAX_PROCESSES - 1u); ++i) {
        char name[32] = {'p','r','o','c','-','0','0','0','\0'};
        name[5] = static_cast<char>('0' + ((i / 100u) % 10u));
        name[6] = static_cast<char>('0' + ((i / 10u) % 10u));
        name[7] = static_cast<char>('0' + (i % 10u));
        if (!StNtCreateProcess(name,
                0x01000000ULL + static_cast<uint64_t>(i) * PAGE_SIZE)) {
            capacity_ok = false;
            break;
        }
    }
    check(capacity_ok && process_count() == MAX_PROCESSES,
          "bounded process registry fills to its configured capacity");
    check(!StNtCreateProcess("overflow", 0x02000000ULL),
          "process registry refuses overflow without reusing terminated slots");

    process_init();
    bool thread_capacity_ok = true;
    for (uint32_t p = 0; p < (MAX_THREADS / MAX_THREADS_PER_PROCESS); ++p) {
        char name[32] = {'t','h','r','-','0','0','0','\0'};
        name[4] = static_cast<char>('0' + ((p / 100u) % 10u));
        name[5] = static_cast<char>('0' + ((p / 10u) % 10u));
        name[6] = static_cast<char>('0' + (p % 10u));
        StProcess* owner = StNtCreateProcess(
            name, 0x03000000ULL + static_cast<uint64_t>(p) * 0x10000ULL);
        if (!owner) { thread_capacity_ok = false; break; }
        for (uint32_t t = 0; t < MAX_THREADS_PER_PROCESS; ++t) {
            const uint64_t stack_top =
                0x01000000ULL +
                (static_cast<uint64_t>(p) * MAX_THREADS_PER_PROCESS + t) * 0x10000ULL;
            if (!StNtCreateThread(owner, USER_IMAGE_BASE,
                                  USER_STACK_TOP, stack_top)) {
                thread_capacity_ok = false;
                break;
            }
        }
        if (!thread_capacity_ok) break;
        check(owner->thread_count == MAX_THREADS_PER_PROCESS,
              "per-process thread limit is enforced");
    }
    check(thread_capacity_ok && thread_count() == MAX_THREADS,
          "bounded thread registry fills to its configured capacity");
    check(!StNtCreateThread(process_find(5ULL), USER_IMAGE_BASE,
                            USER_STACK_TOP, 0x7F000000ULL),
          "global thread registry refuses overflow");

    std::printf("USER_MODE_PHASE2A_SELFTEST: %s (%u checks, %u failures)\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
