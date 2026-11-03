#include "../kernel/src/process/context_switch_policy.hpp"
#include <cstdio>
#include <cstring>

using namespace starlix::process;
using namespace starlix::process::context_switch;
using namespace starlix::mem::user_address;
using starlix::cpu::InterruptReturnFrame;

static unsigned checks = 0;
static unsigned failures = 0;

static void check(bool condition, const char* name) {
    ++checks;
    if (condition) std::printf("[PASS] %s\n", name);
    else {
        ++failures;
        std::printf("[FAIL] %s\n", name);
    }
}

alignas(4096) static uint8_t kernel_stack[8192];

static StThread make_valid_thread() {
    StThread thread{};
    static StProcess process{};
    process = StProcess{};
    process.pid = 9;
    process.page_directory_cr3 = 0x900000ULL;
    process.state = PROCESS_STATE_READY;
    thread.tid = 17;
    thread.priority = 8;
    thread.state = THREAD_STATE_READY;
    thread.kernel_stack_base = reinterpret_cast<uint64_t>(kernel_stack);
    thread.kernel_stack_size = sizeof(kernel_stack);
    thread.kernel_stack_top = thread.kernel_stack_base + sizeof(kernel_stack);
    thread.parent_process = &process;
    thread.saved_interrupt_rsp = thread.kernel_stack_top -
        starlix::cpu::INTERRUPT_RETURN_FRAME_SIZE;
    auto* frame = reinterpret_cast<InterruptReturnFrame*>(
        static_cast<uintptr_t>(thread.saved_interrupt_rsp));
    starlix::cpu::build_initial_user_interrupt_frame(
        *frame, USER_IMAGE_BASE, USER_STACK_TOP);
    return thread;
}

int main() {
    StThread thread = make_valid_thread();
    auto* frame = reinterpret_cast<InterruptReturnFrame*>(
        static_cast<uintptr_t>(thread.saved_interrupt_rsp));

    check((reinterpret_cast<uintptr_t>(thread.fpu_state.bytes) & 0xFu) == 0ULL,
          "each thread FPU state buffer is 16-byte aligned");
    check(sizeof(thread.fpu_state.bytes) == 512u,
          "thread FPU state has the architectural 512-byte FXSAVE area");
    check(valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "valid prepared IRETQ frame is contained in its private kernel stack");
    check(valid_thread_switch_state(thread),
          "valid READY thread has a switchable saved user context");

    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp + 1ULL),
          "unaligned saved frame pointer is rejected");
    check(!valid_saved_user_frame_bounds(thread, thread.kernel_stack_base),
          "frame outside saved top-of-stack range is rejected");

    StThread bad = thread;
    bad.kernel_stack_base += 1ULL;
    check(!valid_saved_user_frame_bounds(bad, bad.saved_interrupt_rsp),
          "unaligned kernel stack base is rejected");
    bad = thread;
    bad.kernel_stack_size -= 1ULL;
    check(!valid_saved_user_frame_bounds(bad, bad.saved_interrupt_rsp),
          "non-page-aligned kernel stack size is rejected");
    bad = thread;
    bad.kernel_stack_size = 2ULL * 1024ULL * 1024ULL;
    check(!valid_saved_user_frame_bounds(bad, bad.saved_interrupt_rsp),
          "kernel stack larger than the configured maximum is rejected");
    bad = thread;
    bad.kernel_stack_top -= 16ULL;
    check(!valid_saved_user_frame_bounds(bad, bad.saved_interrupt_rsp),
          "kernel stack top that disagrees with base plus size is rejected");
    bad = thread;
    bad.kernel_stack_base = ~0ULL - 0xFFFULL;
    bad.kernel_stack_size = 0x2000ULL;
    bad.kernel_stack_top = 0x1000ULL;
    check(!valid_saved_user_frame_bounds(bad, bad.saved_interrupt_rsp),
          "overflowing kernel stack extent is rejected");

    const uint64_t original_vector = frame->vector;
    frame->vector = 33ULL;
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "non-timer saved interrupt frame is rejected for timer switching");
    frame->vector = original_vector;

    const uint64_t original_cs = frame->cs;
    frame->cs = starlix::cpu::KERNEL_CODE_SEGMENT;
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "Ring-0 return frame is rejected by user-thread switch validation");
    frame->cs = original_cs;

    const uint64_t original_ss = frame->ss;
    frame->ss = starlix::cpu::KERNEL_DATA_SEGMENT;
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "kernel SS is rejected by user-thread switch validation");
    frame->ss = original_ss;

    const uint64_t original_rip = frame->rip;
    frame->rip = 0x200000ULL;
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "kernel identity RIP is rejected for a user return frame");
    frame->rip = original_rip;

    const uint64_t original_rsp = frame->rsp;
    frame->rsp = USER_STACK_TOP - 8ULL;
    check(valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "timer-saved live user frame accepts a valid unaligned RSP");
    frame->rsp = USER_REGION_BASE - 16ULL;
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "user RSP outside the reserved region is rejected");
    frame->rsp = original_rsp;

    const uint64_t original_flags = frame->rflags;
    frame->rflags |= (3ULL << 12);
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "user IOPL elevation is rejected before IRETQ");
    frame->rflags = original_flags;
    frame->rflags |= (1ULL << 14);
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "nested-task RFLAGS state is rejected before IRETQ");
    frame->rflags = original_flags;
    frame->rflags |= (1ULL << 17);
    check(!valid_saved_user_frame_bounds(thread, thread.saved_interrupt_rsp),
          "virtual-8086 RFLAGS state is rejected before IRETQ");
    frame->rflags = original_flags;

    bad = thread;
    bad.tid = 0ULL;
    check(!valid_thread_switch_state(bad),
          "zero TID is rejected as a scheduler switch candidate");
    bad = thread;
    bad.state = THREAD_STATE_RUNNING;
    check(!valid_thread_switch_state(bad),
          "already-running thread cannot be selected as a READY candidate");
    bad = thread;
    bad.priority = 32u;
    check(!valid_thread_switch_state(bad),
          "priority outside the scheduler's range is rejected");
    bad = thread;
    bad.parent_process->state = PROCESS_STATE_TERMINATED;
    check(!valid_thread_switch_state(bad),
          "thread owned by a terminated process is rejected");

    bad = thread;
    bad.parent_process->state = PROCESS_STATE_CREATED;
    check(!valid_thread_switch_state(bad),
          "thread owned by an unstarted process is rejected");
    bad = thread;
    bad.parent_process->state = PROCESS_STATE_SYSTEM;
    check(!valid_thread_switch_state(bad),
          "kernel System process cannot be selected as a user thread");
    bad = thread;
    bad.parent_process->state = PROCESS_STATE_READY;
    bad.parent_process->page_directory_cr3 = 0ULL;
    check(!valid_thread_switch_state(bad),
          "kernel-root or zero-CR3 process is rejected as a user target");
    bad = thread;
    bad.parent_process->state = PROCESS_STATE_READY;
    bad.parent_process->page_directory_cr3 |= 1ULL;
    check(!valid_thread_switch_state(bad),
          "unaligned target CR3 is rejected");
    bad = thread;
    bad.parent_process->state = PROCESS_STATE_READY;
    bad.saved_interrupt_rsp = 0ULL;
    check(!valid_thread_switch_state(bad),
          "thread without a prepared interrupt frame is rejected");

    std::printf("CONTEXT_SWITCH_PHASE2F_SELFTEST: %s (%u checks, %u failures)\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
