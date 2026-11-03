// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "context_switch.hpp"
#include "context_switch_policy.hpp"
#include "scheduler.hpp"
#include "../cpu/fpu.hpp"
#include "../cpu/gdt.hpp"
#include "../cpu/idt.hpp"
#include "../mem/address_space.hpp"
#include "../mem/kernel_stack.hpp"
#include "../drivers/serial.hpp"

namespace starlix::process::context_switch {
namespace {

static StThread* g_current_thread = nullptr;
static volatile uint32_t g_initialized = 0u;
static volatile uint32_t g_armed = 0u;
static volatile uint64_t g_switch_count = 0ULL;

static void serial_line(const char* text) {
    starlix::drivers::serial::print(text);
    starlix::drivers::serial::print("\r\n");
}

static uint64_t irq_save_disable() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0ULL)
        asm volatile("sti" ::: "memory");
}

static bool interrupts_are_disabled() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) == 0ULL;
}

static bool valid_owner(StThread* thread, StProcess*& out_process) {
    out_process = nullptr;
    if (!thread_is_registered(thread) || !thread->parent_process ||
        !process_is_registered(thread->parent_process)) return false;
    out_process = thread->parent_process;
    return out_process->state == PROCESS_STATE_READY ||
           out_process->state == PROCESS_STATE_RUNNING;
}

static bool validate_user_base(const StProcess& process, uint64_t base) {
    if (base == 0ULL) return true;
    if (!starlix::mem::user_address::is_user_address(base)) return false;
    starlix::mem::AddressSpace space{};
    space.cr3 = process.page_directory_cr3;
    space.mapped_pages = 0u;
    space.magic = starlix::mem::ADDRESS_SPACE_MAGIC;
    uint64_t physical = 0ULL;
    uint32_t permissions = 0u;
    return starlix::mem::address_space_translate_user(
               space, base, physical, permissions) &&
           (permissions & starlix::mem::user_address::USER_PAGE_READ) != 0u;
}

static bool validate_user_mapping(const StProcess& process,
                                  const starlix::cpu::InterruptReturnFrame& frame) {
    using namespace starlix::mem;
    using namespace user_address;
    if (!address_space_root_registered(process.page_directory_cr3) ||
        frame.rsp == 0ULL || frame.rsp <= USER_REGION_BASE) return false;

    AddressSpace space{};
    space.cr3 = process.page_directory_cr3;
    space.mapped_pages = 0u;
    space.magic = ADDRESS_SPACE_MAGIC;

    uint64_t physical = 0ULL;
    uint32_t permissions = 0u;
    if (!address_space_translate_user(space, frame.rip, physical, permissions) ||
        (permissions & USER_PAGE_EXEC) == 0u) return false;
    if (!address_space_translate_user(space, frame.rsp - 1ULL,
                                      physical, permissions) ||
        (permissions & USER_PAGE_WRITE) == 0u) return false;
    return true;
}

static bool validate_thread_target(StThread* thread,
                                   StProcess*& out_process,
                                   starlix::cpu::InterruptReturnFrame*& out_frame) {
    out_process = nullptr;
    out_frame = nullptr;
    if (!valid_owner(thread, out_process) ||
        !valid_thread_switch_state(*thread)) return false;

    const uint64_t saved = thread->saved_interrupt_rsp;
    if (!valid_saved_user_frame_bounds(*thread, saved)) return false;
    out_frame = reinterpret_cast<starlix::cpu::InterruptReturnFrame*>(
        static_cast<uintptr_t>(saved));
    if (!starlix::mem::kernel_stack_thread_valid(
            thread, out_process->page_directory_cr3) ||
        !validate_user_mapping(*out_process, *out_frame) ||
        !validate_user_base(*out_process, thread->fs_base) ||
        !validate_user_base(*out_process, thread->user_gs_base)) return false;
    if (thread->fpu_state.initialized != 1u &&
        !starlix::cpu::fpu_state_reset(thread->fpu_state)) return false;
    return true;
}

extern "C" [[noreturn]] void starlix_enter_user_from_frame(uint64_t frame);
extern "C" uint8_t starlix_syscall_cpu_local;

} // namespace

void context_switch_init() {
    const uint64_t flags = irq_save_disable();
    g_current_thread = nullptr;
    __atomic_store_n(&g_switch_count, 0ULL, __ATOMIC_RELEASE);
    __atomic_store_n(&g_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_initialized, 1u, __ATOMIC_RELEASE);
    irq_restore(flags);
}

bool context_switch_ready() {
    return __atomic_load_n(&g_initialized, __ATOMIC_ACQUIRE) != 0u &&
           scheduler::scheduler_ready() &&
           starlix::cpu::fpu_ready() &&
           starlix::mem::address_space_manager_ready() &&
           starlix::mem::kernel_stack_manager_ready() &&
           starlix::cpu::timer_is_ready();
}

bool context_switch_armed() {
    return __atomic_load_n(&g_armed, __ATOMIC_ACQUIRE) != 0u;
}

StThread* context_switch_current() {
    return __atomic_load_n(&g_current_thread, __ATOMIC_ACQUIRE);
}

uint64_t context_switch_switch_count() {
    return __atomic_load_n(&g_switch_count, __ATOMIC_ACQUIRE);
}

bool context_switch_start_first_user(StThread* first_thread) {
    using starlix::cpu::InterruptReturnFrame;
    const uint64_t flags = irq_save_disable();

    if (!context_switch_ready() || context_switch_armed() ||
        context_switch_current() != nullptr ||
        !thread_is_registered(first_thread) ||
        first_thread->state != THREAD_STATE_READY ||
        !scheduler::scheduler_is_admitted(first_thread)) {
        serial_line("[CONTEXT] FIRST_USER_PREFLIGHT=FAIL; reason=COORDINATOR_OR_SCHEDULER_STATE");
        irq_restore(flags);
        return false;
    }

    StProcess* first_process = nullptr;
    InterruptReturnFrame* first_frame = nullptr;
    if (!validate_thread_target(first_thread, first_process, first_frame) ||
        first_process->state != PROCESS_STATE_READY) {
        serial_line("[CONTEXT] FIRST_USER_PREFLIGHT=FAIL; reason=THREAD_OR_FRAME_VALIDATION");
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_PREFLIGHT=PASS");

    const uint64_t old_root = starlix::mem::address_space_active_root();
    const uint64_t old_rsp0 = starlix::cpu::tss_get_kernel_stack();
    const uint64_t old_fs = starlix::cpu::rdmsr(starlix::cpu::MSR_FS_BASE);
    const uint64_t old_gs = starlix::cpu::rdmsr(starlix::cpu::MSR_GS_BASE);
    const uint64_t old_kernel_gs =
        starlix::cpu::rdmsr(starlix::cpu::MSR_KERNEL_GS_BASE);
    const uint64_t kernel_gs = reinterpret_cast<uint64_t>(
        &starlix_syscall_cpu_local);
    if (old_root != starlix::mem::address_space_kernel_root() ||
        old_rsp0 == 0ULL || kernel_gs == 0ULL) {
        serial_line("[CONTEXT] FIRST_USER_MACHINE_STATE=FAIL; reason=KERNEL_CR3_OR_TSS_OR_GS");
        irq_restore(flags);
        return false;
    }

    starlix::cpu::FpuState rollback_fpu{};
    if (!starlix::cpu::fpu_state_save(rollback_fpu)) {
        serial_line("[CONTEXT] FIRST_USER_FPU_SNAPSHOT=FAIL");
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_FPU_SNAPSHOT=PASS");

    const auto rollback_machine_state = [&]() {
        starlix::cpu::wrmsr(starlix::cpu::MSR_FS_BASE, old_fs);
        starlix::cpu::wrmsr(starlix::cpu::MSR_GS_BASE, old_gs);
        starlix::cpu::wrmsr(starlix::cpu::MSR_KERNEL_GS_BASE, old_kernel_gs);
        const bool root_restored =
            starlix::mem::address_space_activate_kernel() &&
            starlix::mem::address_space_active_root() == old_root;
        const bool stack_restored =
            starlix::cpu::tss_set_kernel_stack_checked(old_rsp0) &&
            starlix::cpu::tss_get_kernel_stack() == old_rsp0;
        const bool fpu_restored =
            starlix::cpu::fpu_state_restore(rollback_fpu);
        const bool msrs_restored =
            starlix::cpu::rdmsr(starlix::cpu::MSR_FS_BASE) == old_fs &&
            starlix::cpu::rdmsr(starlix::cpu::MSR_GS_BASE) == old_gs &&
            starlix::cpu::rdmsr(starlix::cpu::MSR_KERNEL_GS_BASE) == old_kernel_gs;
        return root_restored && stack_restored && fpu_restored && msrs_restored;
    };
    const auto rollback_or_fail_stop = [&]() {
        serial_line("[CONTEXT] FIRST_USER_ROLLBACK=BEGIN");
        if (rollback_machine_state()) {
            serial_line("[CONTEXT] FIRST_USER_ROLLBACK=PASS");
            return;
        }
        /* Never return to an unknown CR3/TSS/GS/FPU combination. */
        serial_line("[FATAL] FIRST_USER_ROLLBACK=FAIL; entering CLI_HLT fail-stop");
        asm volatile("cli" ::: "memory");
        for (;;) asm volatile("hlt" ::: "memory");
    };

    if (!starlix::cpu::tss_set_kernel_stack_checked(
            first_thread->kernel_stack_top)) {
        serial_line("[CONTEXT] FIRST_USER_TSS_RSP0=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_TSS_RSP0=PASS");
    if (!starlix::mem::address_space_activate_root(
            first_process->page_directory_cr3)) {
        serial_line("[CONTEXT] FIRST_USER_CR3_SWITCH=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_CR3_SWITCH=PASS");
    if (!starlix::cpu::fpu_state_restore(first_thread->fpu_state)) {
        serial_line("[CONTEXT] FIRST_USER_FPU_RESTORE=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_FPU_RESTORE=PASS");
    if (starlix::mem::address_space_active_root() !=
            first_process->page_directory_cr3 ||
        starlix::cpu::tss_get_kernel_stack() != first_thread->kernel_stack_top) {
        serial_line("[CONTEXT] FIRST_USER_CR3_TSS_VERIFY=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_CR3_TSS_VERIFY=PASS");

    starlix::cpu::wrmsr(starlix::cpu::MSR_GS_BASE, kernel_gs);
    starlix::cpu::wrmsr(starlix::cpu::MSR_FS_BASE, first_thread->fs_base);
    starlix::cpu::wrmsr(starlix::cpu::MSR_KERNEL_GS_BASE,
                        first_thread->user_gs_base);
    if (starlix::cpu::rdmsr(starlix::cpu::MSR_GS_BASE) != kernel_gs ||
        starlix::cpu::rdmsr(starlix::cpu::MSR_FS_BASE) != first_thread->fs_base ||
        starlix::cpu::rdmsr(starlix::cpu::MSR_KERNEL_GS_BASE) !=
            first_thread->user_gs_base) {
        serial_line("[CONTEXT] FIRST_USER_SEGMENT_BASES=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_SEGMENT_BASES=PASS");

    /* Remove the admitted READY entry before changing either lifecycle state.
       With IF masked on this single-CPU kernel, this reservation cannot race. */
    if (!scheduler::scheduler_remove(first_thread->tid)) {
        serial_line("[CONTEXT] FIRST_USER_SCHEDULER_REMOVE=FAIL");
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_SCHEDULER_REMOVE=PASS");

    if (!process_set_state(first_process, PROCESS_STATE_RUNNING)) {
        serial_line("[CONTEXT] FIRST_USER_PROCESS_STATE=FAIL");
        (void)scheduler::scheduler_admit(first_thread);
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_PROCESS_STATE=RUNNING");
    if (!thread_set_state(first_thread, THREAD_STATE_RUNNING)) {
        serial_line("[CONTEXT] FIRST_USER_THREAD_STATE=FAIL");
        (void)process_set_state(first_process, PROCESS_STATE_READY);
        (void)scheduler::scheduler_admit(first_thread);
        rollback_or_fail_stop();
        irq_restore(flags);
        return false;
    }
    serial_line("[CONTEXT] FIRST_USER_THREAD_STATE=RUNNING");
    __atomic_store_n(&g_current_thread, first_thread, __ATOMIC_RELEASE);
    __atomic_store_n(&g_armed, 1u, __ATOMIC_RELEASE);
    /* This is the last kernel breadcrumb before the non-returning CPL3 IRETQ. */
    serial_line("[CONTEXT] IRETQ_DISPATCH=BEGIN");
    /* IRETQ consumes the prepared 168-byte frame and enables user RFLAGS.IF. */
    starlix_enter_user_from_frame(
        reinterpret_cast<uint64_t>(first_frame));
}

uint64_t context_switch_on_timer(
    starlix::cpu::InterruptReturnFrame* current_frame
) {
    /* The IRQ gate must have masked IF; otherwise the CR3/TSS transition is
       not atomic against another interrupt on this single-CPU kernel. */
    if (!interrupts_are_disabled() || !context_switch_armed() || !current_frame ||
        (current_frame->cs & 3ULL) != 3ULL ||
        current_frame->vector != starlix::cpu::INTERRUPT_FRAME_VECTOR_TIMER) {
        return 0ULL;
    }

    StThread* current = context_switch_current();
    StProcess* current_process = nullptr;
    if (!thread_is_registered(current) ||
        current->state != THREAD_STATE_RUNNING ||
        !valid_owner(current, current_process) ||
        current_process->state != PROCESS_STATE_RUNNING ||
        !valid_saved_user_frame_bounds(
            *current, reinterpret_cast<uint64_t>(current_frame)) ||
        !starlix::mem::kernel_stack_thread_valid(
            current, current_process->page_directory_cr3) ||
        starlix::mem::address_space_active_root() !=
            current_process->page_directory_cr3 ||
        starlix::cpu::tss_get_kernel_stack() != current->kernel_stack_top) {
        return 0ULL;
    }

    const uint64_t outgoing_fs =
        starlix::cpu::rdmsr(starlix::cpu::MSR_FS_BASE);
    const uint64_t outgoing_gs =
        starlix::cpu::rdmsr(starlix::cpu::MSR_KERNEL_GS_BASE);
    if (!validate_user_base(*current_process, outgoing_fs) ||
        !validate_user_base(*current_process, outgoing_gs)) return 0ULL;
    current->fs_base = outgoing_fs;
    current->user_gs_base = outgoing_gs;
    current->saved_interrupt_rsp = reinterpret_cast<uint64_t>(current_frame);
    if (!starlix::cpu::fpu_state_save(current->fpu_state)) return 0ULL;
    if (!thread_set_state(current, THREAD_STATE_READY)) return 0ULL;
    if (!scheduler::scheduler_admit(current)) {
        (void)thread_set_state(current, THREAD_STATE_RUNNING);
        (void)starlix::cpu::fpu_state_restore(current->fpu_state);
        return 0ULL;
    }

    const auto resume_current = [&]() {
        (void)scheduler::scheduler_remove(current->tid);
        if (current->state == THREAD_STATE_READY)
            (void)thread_set_state(current, THREAD_STATE_RUNNING);
        (void)starlix::cpu::fpu_state_restore(current->fpu_state);
        starlix::cpu::wrmsr(starlix::cpu::MSR_FS_BASE, current->fs_base);
        starlix::cpu::wrmsr(starlix::cpu::MSR_KERNEL_GS_BASE,
                            current->user_gs_base);
        return 0ULL;
    };

    for (uint32_t attempt = 0u;
         attempt < scheduler::MAX_READY_THREADS; ++attempt) {
        StThread* next = scheduler::scheduler_pick_next();
        if (!next) break;

        if (next == current) {
            (void)scheduler::scheduler_remove(current->tid);
            (void)thread_set_state(current, THREAD_STATE_RUNNING);
            (void)starlix::cpu::fpu_state_restore(current->fpu_state);
            return 0ULL;
        }

        StProcess* next_process = nullptr;
        starlix::cpu::InterruptReturnFrame* next_frame = nullptr;
        if (!validate_thread_target(next, next_process, next_frame)) {
            (void)scheduler::scheduler_remove(next->tid);
            continue;
        }

        const uint64_t old_root = current_process->page_directory_cr3;
        const uint64_t old_rsp0 = current->kernel_stack_top;
        const bool different_process = next_process != current_process;
        bool old_process_changed = false;
        bool next_process_changed = false;

        if (!scheduler::scheduler_remove(next->tid)) continue;
        if (!starlix::cpu::tss_set_kernel_stack_checked(
                next->kernel_stack_top)) {
            (void)scheduler::scheduler_admit(next);
            continue;
        }
        if (!starlix::mem::address_space_activate_root(
                next_process->page_directory_cr3)) {
            (void)starlix::cpu::tss_set_kernel_stack_checked(old_rsp0);
            (void)scheduler::scheduler_admit(next);
            continue;
        }
        if (!starlix::cpu::fpu_state_restore(next->fpu_state)) {
            (void)starlix::mem::address_space_activate_root(old_root);
            (void)starlix::cpu::tss_set_kernel_stack_checked(old_rsp0);
            (void)scheduler::scheduler_admit(next);
            return resume_current();
        }
        starlix::cpu::wrmsr(starlix::cpu::MSR_FS_BASE, next->fs_base);
        starlix::cpu::wrmsr(starlix::cpu::MSR_KERNEL_GS_BASE,
                            next->user_gs_base);
        if (starlix::cpu::rdmsr(starlix::cpu::MSR_FS_BASE) != next->fs_base ||
            starlix::cpu::rdmsr(starlix::cpu::MSR_KERNEL_GS_BASE) !=
                next->user_gs_base) {
            (void)starlix::mem::address_space_activate_root(old_root);
            (void)starlix::cpu::tss_set_kernel_stack_checked(old_rsp0);
            (void)scheduler::scheduler_admit(next);
            return resume_current();
        }

        bool committed = true;
        if (different_process &&
            current_process->state == PROCESS_STATE_RUNNING) {
            old_process_changed = process_set_state(
                current_process, PROCESS_STATE_READY);
            committed = old_process_changed;
        }
        if (committed && different_process &&
            next_process->state == PROCESS_STATE_READY) {
            next_process_changed = process_set_state(
                next_process, PROCESS_STATE_RUNNING);
            committed = next_process_changed;
        }
        if (committed)
            committed = thread_set_state(next, THREAD_STATE_RUNNING);

        if (!committed) {
            if (next_process_changed)
                (void)process_set_state(next_process, PROCESS_STATE_READY);
            if (old_process_changed)
                (void)process_set_state(current_process, PROCESS_STATE_RUNNING);
            (void)starlix::mem::address_space_activate_root(old_root);
            (void)starlix::cpu::tss_set_kernel_stack_checked(old_rsp0);
            (void)scheduler::scheduler_admit(next);
            return resume_current();
        }

        __atomic_store_n(&g_current_thread, next, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_switch_count, 1ULL, __ATOMIC_ACQ_REL);
        return reinterpret_cast<uint64_t>(next_frame);
    }

    /* No eligible peer: remove/reselect the current thread and keep running. */
    (void)scheduler::scheduler_remove(current->tid);
    if (current->state == THREAD_STATE_READY)
        (void)thread_set_state(current, THREAD_STATE_RUNNING);
    (void)starlix::cpu::fpu_state_restore(current->fpu_state);
    return 0ULL;
}

} // namespace starlix::process::context_switch
