// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "user_mode_smoke_test.hpp"
#include "user_probe_payload.hpp"
#include "process.hpp"
#include "scheduler.hpp"
#include "context_switch.hpp"
#include "../cpu/interrupt_frame.hpp"
#include "../cpu/gdt.hpp"
#include "../cpu/idt.hpp"
#include "../cpu/msr.hpp"
#include "../drivers/serial.hpp"
#include "../mem/address_space.hpp"
#include "../mem/kernel_stack.hpp"
#include "../mem/managed_page_pool.hpp"
#include "../mem/phys.hpp"

namespace starlix::process::user_mode_smoke_test {
namespace {
using namespace starlix::mem;
using namespace starlix::mem::user_address;
static const char* g_failure_reason = "NOT_RUN";

#if STARLIX_ENABLE_RING3_SMOKE_TEST
constexpr uint64_t PROBE_CODE_VA = USER_IMAGE_BASE;
constexpr uint64_t PROBE_COUNTER_VA = USER_IMAGE_BASE + PAGE_SIZE;
constexpr uint64_t PROBE_COUNTER0_VA = PROBE_COUNTER_VA;
constexpr uint64_t PROBE_COUNTER1_VA = PROBE_COUNTER_VA + sizeof(uint64_t);
constexpr uint64_t PROBE_EXIT_RIP = USER_IMAGE_BASE +
    user_probe::EXIT_RIP_OFFSET;

static uint64_t g_counter_physical = 0ULL;
static uint64_t g_process_a_pid = 0ULL;
static uint64_t g_process_b_pid = 0ULL;
static volatile uint32_t g_probe_armed = 0u;

static bool map_user_image(AddressSpace& space, uint64_t code_physical,
                           uint64_t stack_physical, uint64_t counter_physical) {
    if (!address_space_map_user_page(space, PROBE_CODE_VA, code_physical,
            USER_PAGE_READ | USER_PAGE_EXEC)) return false;
    if (!address_space_map_user_page(space, PROBE_COUNTER_VA, counter_physical,
            USER_PAGE_READ | USER_PAGE_WRITE)) return false;
    for (uint64_t page = 0; page < USER_STACK_PAGES; ++page) {
        const uint64_t va = USER_STACK_BASE + page * PAGE_SIZE;
        const uint64_t pa = stack_physical + page * PAGE_SIZE;
        if (!address_space_map_user_page(space, va, pa,
                USER_PAGE_READ | USER_PAGE_WRITE)) return false;
    }
    return true;
}

static bool allocate_and_copy_probe(uint64_t counter_phys,
                                    uint64_t& out_code_a,
                                    uint64_t& out_code_b) {
    out_code_a = out_code_b = 0ULL;
    out_code_a = alloc_phys_pages(1ULL);
    out_code_b = alloc_phys_pages(1ULL);
    if (!out_code_a || !out_code_b) return false;
    auto* code_a = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out_code_a));
    auto* code_b = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(out_code_b));
    if (!user_probe::build_payload(code_a, PAGE_SIZE,
            PROBE_COUNTER0_VA, PROBE_COUNTER1_VA) ||
        !user_probe::build_payload(code_b, PAGE_SIZE,
            PROBE_COUNTER1_VA, PROBE_COUNTER0_VA)) return false;
    auto* counters = reinterpret_cast<volatile uint64_t*>(
        static_cast<uintptr_t>(counter_phys));
    counters[0] = 0ULL;
    counters[1] = 0ULL;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return true;
}
#endif

#if STARLIX_ENABLE_RING3_SMOKE_TEST
static void halt_forever() __attribute__((noreturn));
static void halt_forever() {
    asm volatile("cli" ::: "memory");
    for (;;) asm volatile("hlt" ::: "memory");
}
#endif

} // namespace

bool enabled() {
#if STARLIX_ENABLE_RING3_SMOKE_TEST
    return true;
#else
    return false;
#endif
}

const char* failure_reason() { return g_failure_reason; }

bool start() {
#if !STARLIX_ENABLE_RING3_SMOKE_TEST
    g_failure_reason = "DIAGNOSTIC_NOT_COMPILED_IN";
    return false;
#else
    g_failure_reason = "PRECONDITION_CONTEXT_SWITCH";
    if (!context_switch::context_switch_ready()) return false;
    g_failure_reason = "PRECONDITION_KERNEL_STACK_ARENA";
    if (!kernel_stack_manager_ready()) return false;
    g_failure_reason = "PRECONDITION_PHYSICAL_ALLOCATOR";
    if (!phys_allocator_ready()) return false;
    g_failure_reason = "PRECONDITION_MANAGED_PAGE_POOL";
    if (!managed_page_pool_ready()) return false;
    g_failure_reason = "PRECONDITION_ADDRESS_SPACE_MANAGER";
    if (!address_space_manager_ready()) return false;
    g_failure_reason = "PRECONDITION_SCHEDULER";
    if (!scheduler::scheduler_ready()) return false;
    g_failure_reason = "PRECONDITION_FPU_SSE";
    if (!starlix::cpu::fpu_ready()) return false;
    g_failure_reason = "PRECONDITION_PIT";
    if (!starlix::cpu::timer_is_ready()) return false;

    AddressSpace space_a{};
    AddressSpace space_b{};
    g_failure_reason = "ADDRESS_SPACE_CLONE";
    if (!address_space_create(space_a) || !address_space_create(space_b)) return false;

    const uint64_t counter_phys = alloc_phys_pages(1ULL);
    const uint64_t stack_a_phys = alloc_phys_pages(USER_STACK_PAGES);
    const uint64_t stack_b_phys = alloc_phys_pages(USER_STACK_PAGES);
    uint64_t code_a_phys = 0ULL;
    uint64_t code_b_phys = 0ULL;
    g_failure_reason = "MANAGED_TEST_IMAGE_PAGE_ALLOCATION_OR_PAYLOAD";
    if (!counter_phys || !stack_a_phys || !stack_b_phys ||
        !allocate_and_copy_probe(counter_phys, code_a_phys, code_b_phys)) return false;

    g_failure_reason = "USER_RX_CODE_RW_NX_STACK_OR_SHARED_COUNTER_MAPPING";
    if (!map_user_image(space_a, code_a_phys, stack_a_phys, counter_phys) ||
        !map_user_image(space_b, code_b_phys, stack_b_phys, counter_phys))
        return false;

    g_failure_reason = "PROCESS_A_REGISTRATION";
    StProcess* process_a = StNtCreateProcess("Ring3ProbeA", space_a.cr3);
    if (!process_a) return false;
    g_failure_reason = "PROCESS_B_REGISTRATION";
    StProcess* process_b = StNtCreateProcess("Ring3ProbeB", space_b.cr3);
    if (!process_b) return false;

    KernelStack kernel_stack_a{};
    KernelStack kernel_stack_b{};
    g_failure_reason = "GUARDED_KERNEL_STACK_A_ALLOCATION";
    if (!kernel_stack_allocate_default(kernel_stack_a)) return false;
    g_failure_reason = "GUARDED_KERNEL_STACK_B_ALLOCATION";
    if (!kernel_stack_allocate_default(kernel_stack_b)) return false;

    g_failure_reason = "THREAD_A_CREATE";
    StThread* thread_a = StNtCreateThread(
        process_a, PROBE_CODE_VA, USER_STACK_TOP, kernel_stack_a.top);
    if (!thread_a) return false;
    g_failure_reason = "THREAD_B_CREATE";
    StThread* thread_b = StNtCreateThread(
        process_b, PROBE_CODE_VA, USER_STACK_TOP, kernel_stack_b.top);
    if (!thread_b) return false;
    g_failure_reason = "KERNEL_STACK_A_BIND_OR_INITIAL_FRAME";
    if (!kernel_stack_bind_thread(kernel_stack_a, thread_a)) {
        g_failure_reason = kernel_stack_failure_reason();
        return false;
    }
    g_failure_reason = "KERNEL_STACK_B_BIND_OR_INITIAL_FRAME";
    if (!kernel_stack_bind_thread(kernel_stack_b, thread_b)) {
        g_failure_reason = kernel_stack_failure_reason();
        return false;
    }
    g_failure_reason = "SCHEDULER_ADMIT_THREAD_A";
    if (!scheduler::scheduler_admit(thread_a)) return false;
    g_failure_reason = "SCHEDULER_ADMIT_THREAD_B";
    if (!scheduler::scheduler_admit(thread_b)) return false;

    g_counter_physical = counter_phys;
    g_process_a_pid = process_a->pid;
    g_process_b_pid = process_b->pid;
    __atomic_store_n(&g_probe_armed, 1u, __ATOMIC_RELEASE);
    starlix::drivers::serial::print(
        "[RING3-TEST] Launching two isolated probe processes; "
        "waiting for timer preemption and INT 0x80 completion.\r\n");
    starlix::drivers::serial::print("[RING3-TEST] SETUP=PASS\r\n");
    g_failure_reason = "FIRST_USER_IRETQ_ENTRY_REJECTED";
    const bool entered = context_switch::context_switch_start_first_user(thread_a);
    if (entered) g_failure_reason = "PASS";
    return entered;
#endif
}

} // namespace starlix::process::user_mode_smoke_test

#if STARLIX_ENABLE_RING3_SMOKE_TEST
extern "C" [[noreturn]] void starlix_user_probe_exit_dispatch(
    const void* raw_frame
) {
    using namespace starlix::cpu;
    using namespace starlix::process;
    using namespace starlix::process::user_mode_smoke_test;

    asm volatile("cli" ::: "memory");
    const auto* frame = static_cast<const InterruptReturnFrame*>(raw_frame);
    StThread* current = context_switch::context_switch_current();
    bool valid = __atomic_load_n(&g_probe_armed, __ATOMIC_ACQUIRE) != 0u &&
        frame && frame->vector == 0x80ULL &&
        frame->cs == USER_CODE_SEGMENT && frame->ss == USER_DATA_SEGMENT &&
        frame->rip == PROBE_EXIT_RIP && frame->rax == user_probe::EXIT_MAGIC &&
        thread_is_registered(current) && current->state == THREAD_STATE_RUNNING &&
        current->parent_process && process_is_registered(current->parent_process) &&
        current->parent_process->state == PROCESS_STATE_RUNNING &&
        (current->parent_process->pid == g_process_a_pid ||
         current->parent_process->pid == g_process_b_pid) &&
        context_switch::context_switch_armed() &&
        context_switch::context_switch_switch_count() >= 1ULL &&
        starlix::mem::address_space_active_root() ==
            current->parent_process->page_directory_cr3 &&
        tss_get_kernel_stack() == current->kernel_stack_top &&
        starlix::mem::kernel_stack_thread_valid(
            current, current->parent_process->page_directory_cr3) &&
        g_counter_physical != 0ULL;

    if (valid) {
        const auto* counters = reinterpret_cast<const volatile uint64_t*>(
            static_cast<uintptr_t>(g_counter_physical));
        valid = counters[0] >= user_probe::COUNTER_THRESHOLD &&
                counters[1] >= user_probe::COUNTER_THRESHOLD;
    }

    if (valid) {
        starlix::drivers::serial::print(
            "[RING3-TEST] PASS: CPL3 IRETQ, user memory isolation, "
            "shared counter page, PIT preemption, alternate IRETQ frame, "
            "CR3 and TSS.RSP0 verified.\r\n");
    } else {
        starlix::drivers::serial::print(
            "[RING3-TEST] FAIL: user transition or timer context-switch "
            "invariant was not satisfied. CPU halted safely.\r\n");
    }
    halt_forever();
}

#endif // STARLIX_ENABLE_RING3_SMOKE_TEST
