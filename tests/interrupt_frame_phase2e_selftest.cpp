#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../kernel/src/cpu/interrupt_frame.hpp"
#include "../kernel/src/process/process.hpp"
#include "../kernel/src/cpu/gdt.hpp"

using namespace starlix::cpu;
using namespace starlix::mem::user_address;
using namespace starlix::process;

extern "C" void asm_load_gdt(uint64_t) {}
extern "C" void asm_load_tss(uint16_t) {}

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

alignas(4096) static uint8_t kernel_stack_a[8192];
alignas(4096) static uint8_t kernel_stack_b[8192];
alignas(4096) static uint8_t tss_test_stack[4096];

static StThread* make_thread(const char* name, uint8_t* stack,
                             uint64_t entry = USER_IMAGE_BASE,
                             uint64_t user_rsp = USER_STACK_TOP) {
    const uint64_t base = reinterpret_cast<uint64_t>(stack);
    const uint64_t size = sizeof(kernel_stack_a);
    StProcess* process = StNtCreateProcess(name, 0x00800000ULL);
    if (!process) return nullptr;
    return StNtCreateThread(process, entry, user_rsp, base + size);
}

int main() {
    check(sizeof(InterruptReturnFrame) == 168ULL,
          "interrupt frame size matches 15 GPRs, vector and IRETQ frame");
    check(__builtin_offsetof(InterruptReturnFrame, vector) == 120ULL &&
          __builtin_offsetof(InterruptReturnFrame, rip) == 128ULL &&
          __builtin_offsetof(InterruptReturnFrame, cs) == 136ULL &&
          __builtin_offsetof(InterruptReturnFrame, rsp) == 152ULL,
          "frame offsets match assembly dispatcher arguments and restore order");

    InterruptReturnFrame frame{};
    build_initial_user_interrupt_frame(frame, USER_IMAGE_BASE, USER_STACK_TOP);
    check(valid_user_interrupt_return_frame(frame),
          "builder creates a valid Ring-3 IRETQ frame");
    check(frame.rip == USER_IMAGE_BASE && frame.rsp == USER_STACK_TOP &&
          frame.cs == USER_CODE_SEGMENT && frame.ss == USER_DATA_SEGMENT &&
          frame.rflags == 0x202ULL && frame.vector == INTERRUPT_FRAME_VECTOR_TIMER,
          "initial frame contains planned user entry, stack, selectors and flags");

    InterruptReturnFrame invalid = frame;
    invalid.cs = KERNEL_CODE_SEGMENT;
    check(!valid_user_interrupt_return_frame(invalid),
          "kernel code selector cannot pass a user return-frame check");
    invalid = frame; invalid.ss = KERNEL_DATA_SEGMENT;
    check(!valid_user_interrupt_return_frame(invalid),
          "kernel data selector cannot pass a user return-frame check");
    invalid = frame; invalid.rip = 0x200000ULL;
    check(!valid_user_interrupt_return_frame(invalid),
          "kernel identity entry point is rejected");
    invalid = frame; invalid.rsp = USER_STACK_TOP - 8ULL;
    check(valid_user_interrupt_return_frame(invalid),
          "live user interrupt frame accepts an ABI-valid unaligned RSP");
    check(!valid_initial_user_interrupt_return_frame(invalid),
          "initial IRETQ frame still requires a 16-byte-aligned user stack");
    invalid = frame; invalid.rflags = 0x002ULL;
    check(valid_user_interrupt_return_frame(invalid),
          "valid interrupt return frame may preserve a cleared IF bit");
    invalid = frame; invalid.rflags |= (3ULL << 12);
    check(!valid_user_interrupt_return_frame(invalid),
          "user IOPL elevation is rejected");
    invalid = frame; invalid.rflags |= (1ULL << 14);
    check(!valid_user_interrupt_return_frame(invalid),
          "nested-task flag is rejected");
    invalid = frame; invalid.rflags |= (1ULL << 17);
    check(!valid_user_interrupt_return_frame(invalid),
          "virtual-8086 return is rejected");
    invalid = frame; invalid.rsp = USER_REGION_END + 16ULL;
    check(!valid_user_interrupt_return_frame(invalid),
          "stack pointer outside the reserved user region is rejected");

    gdt_init();
    check(tss_exception_stacks_ready(),
          "GDT initialization retains the existing dedicated IST stacks");
    const uint64_t tss_top = reinterpret_cast<uint64_t>(tss_test_stack) +
                             sizeof(tss_test_stack);
    check(tss_get_kernel_stack() == 0ULL,
          "fresh TSS has no Ring-0 privilege stack until explicitly configured");
    check(tss_set_kernel_stack_checked(tss_top) &&
          tss_get_kernel_stack() == tss_top,
          "validated TSS RSP0 setter accepts an aligned supervisor stack");
    check(!tss_set_kernel_stack_checked(0ULL) &&
          !tss_set_kernel_stack_checked(tss_top + 1ULL),
          "TSS RSP0 setter rejects null and unaligned stack tops");
    check(!tss_set_kernel_stack_checked(USER_REGION_BASE + 0x1000ULL),
          "TSS RSP0 setter rejects an address inside the user region");
    check(!tss_set_kernel_stack_checked(0x0000800000000000ULL),
          "TSS RSP0 setter rejects non-canonical stack addresses");

    process_init();
    StThread* thread = make_thread("frame-a.ect", kernel_stack_a);
    check(thread != nullptr && thread_is_registered(thread),
          "registered user thread can be prepared");
    const uint64_t base_a = reinterpret_cast<uint64_t>(kernel_stack_a);
    const uint64_t top_a = base_a + sizeof(kernel_stack_a);
    check(thread && thread_prepare_user_interrupt_frame(
              thread, base_a, sizeof(kernel_stack_a)),
          "caller-owned kernel stack receives the initial user interrupt frame");
    const uint64_t saved = thread ? thread_saved_interrupt_frame(thread) : 0ULL;
    check(saved == top_a - sizeof(InterruptReturnFrame),
          "saved frame is placed immediately below the validated kernel stack top");
    check(thread && thread->kernel_stack_base == base_a &&
          thread->kernel_stack_size == sizeof(kernel_stack_a),
          "thread retains exact kernel-stack bounds for future TSS validation");
    if (saved) {
        const auto* stored = reinterpret_cast<const InterruptReturnFrame*>(saved);
        check(valid_user_interrupt_return_frame(*stored),
              "stored frame is valid for the architecture IRETQ restore path");
        check(stored->rip == thread->context.rip &&
              stored->rsp == thread->user_stack,
              "stored frame matches the thread's validated entry and user stack");
    } else {
        check(false, "stored frame is valid for the architecture IRETQ restore path");
        check(false, "stored frame matches the thread's validated entry and user stack");
    }
    check(thread && !thread_prepare_user_interrupt_frame(
              thread, base_a, sizeof(kernel_stack_a)),
          "a thread's initial interrupt frame cannot be prepared twice");
    check(thread && !thread_prepare_user_interrupt_frame(
              thread, base_a, sizeof(kernel_stack_a) - 1ULL),
          "non-page-sized kernel stack range is rejected");
    check(thread && !thread_prepare_user_interrupt_frame(
              thread, base_a + 4096ULL, sizeof(kernel_stack_a)),
          "kernel stack end must match the stack top supplied at thread creation");
    check(!thread_prepare_user_interrupt_frame(
              reinterpret_cast<StThread*>(0x1234ULL), base_a,
              sizeof(kernel_stack_a)),
          "foreign thread object is rejected before dereference");

    StThread* thread_b = make_thread("frame-b.ect", kernel_stack_b);
    const uint64_t base_b = reinterpret_cast<uint64_t>(kernel_stack_b);
    check(thread_b && thread_prepare_user_interrupt_frame(
              thread_b, base_b, sizeof(kernel_stack_b)),
          "a second user thread receives an independent kernel interrupt frame");
    check(thread_b && thread_saved_interrupt_frame(thread_b) != saved,
          "different threads retain different saved kernel-stack frames");
    StProcess* overlap_process = StNtCreateProcess("overlap.ect", 0x00900000ULL);
    StThread* overlapping = overlap_process ? StNtCreateThread(
        overlap_process, USER_IMAGE_BASE, USER_STACK_TOP,
        base_a + 4096ULL + sizeof(kernel_stack_a)) : nullptr;
    check(overlapping && !thread_prepare_user_interrupt_frame(
              overlapping, base_a + 4096ULL, sizeof(kernel_stack_a)),
          "overlapping live kernel stack ranges are rejected");

    StThread* bad_entry = make_thread("bad-entry.ect", kernel_stack_a,
                                      0x200000ULL, USER_STACK_TOP);
    check(bad_entry == nullptr,
          "invalid user entry is rejected before a thread can be registered");

    std::printf("INTERRUPT_FRAME_PHASE2E_SELFTEST: %s (%u checks, %u failures)\n",
                failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
