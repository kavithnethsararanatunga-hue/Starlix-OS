#include <cstdio>
#include "freestanding_host_types.hpp"
#include "../kernel/src/process/process.hpp"
#include "../kernel/src/mem/user_address.hpp"

using namespace starlix::process;
using namespace starlix::mem::user_address;

namespace {
uint32_t passed = 0u;
uint32_t failed = 0u;
void check(bool condition, const char* label) {
    if (condition) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", label); }
}
constexpr uint64_t TEST_KERNEL_STACK_TOP = 0xFFFFFF0000005000ULL;
}

int main() {
    process_init();
    check(process_count() == 1u && thread_count() == 0u,
          "registry starts with only the system process");

    StProcess* process = StNtCreateProcess("stlcs", 0x1000ULL);
    check(process && process->state == PROCESS_STATE_CREATED,
          "creates an unpublished process record");
    StThread* thread = StNtCreateThread(
        process, USER_IMAGE_BASE, USER_STACK_TOP - 16ULL,
        TEST_KERNEL_STACK_TOP);
    check(thread && process->state == PROCESS_STATE_READY &&
          thread->state == THREAD_STATE_READY,
          "creates a READY main thread in the process");
    check(process_count() == 2u && thread_count() == 1u,
          "process and thread appear in bounded registries");
    check(!process_abort_creation(process, nullptr),
          "cannot abort a process while its main thread remains registered");
    check(!process_abort_creation(process, thread),
          "cannot abort a thread while its kernel stack is still attached");

    thread->kernel_stack_top = 0ULL;
    check(process_abort_creation(process, thread),
          "can abort the newest READY process after stack detachment");
    check(!process_is_registered(process) && !thread_is_registered(thread) &&
          process_count() == 1u && thread_count() == 0u,
          "abort removes both process and thread registry entries");

    StProcess* process_without_thread = StNtCreateProcess("no-thread", 0x2000ULL);
    check(process_without_thread &&
          process_abort_creation(process_without_thread, nullptr),
          "can abort a newest process that never acquired a thread");
    check(process_count() == 1u,
          "empty-process abort restores the registry baseline");

    StProcess* older = StNtCreateProcess("older", 0x3000ULL);
    StProcess* newer = StNtCreateProcess("newer", 0x4000ULL);
    check(older && newer, "creates two ordered unpublished process records");
    check(!process_abort_creation(older, nullptr),
          "refuses non-LIFO removal that would create a registry hole");
    check(process_abort_creation(newer, nullptr) &&
          process_abort_creation(older, nullptr),
          "LIFO rollback removes newest then older process cleanly");
    check(process_count() == 1u && thread_count() == 0u,
          "all abort scenarios leave only the system process");

    std::printf("Process abort Phase 2L self-test: %u passed, %u failed\n",
                passed, failed);
    return failed == 0u ? 0 : 1;
}
