#include "../kernel/src/cpu/exception_frame_policy.hpp"
#include "../user_components/stlcs/stlcs_abi.hpp"
#include <cstdio>

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

int main() {
    using namespace starlix::cpu::exception_policy;
    check(valid_kernel_ss(0ULL),
          "long-mode nested IST frame accepts architecturally valid NULL SS");
    check(valid_kernel_ss(starlix::cpu::KERNEL_DATA_SEGMENT),
          "kernel data selector remains valid for bootstrap exceptions");
    check(!valid_kernel_ss(starlix::cpu::USER_DATA_SEGMENT),
          "user data selector is rejected in a kernel exception frame");
    check(!valid_kernel_ss(0x18ULL),
          "unexpected non-kernel SS selector is rejected");

    constexpr uint64_t begin = 0xFFFFFF0000001000ULL;
    constexpr uint64_t size = 4ULL * 4096ULL;
    check(stack_range_contains(begin, begin, size),
          "first byte of active kernel stack is accepted");
    check(stack_range_contains(begin + size, begin, size),
          "kernel stack top is accepted as an interrupted RSP boundary");
    check(!stack_range_contains(begin - 8ULL, begin, size),
          "lower guard-page address is rejected");
    check(!stack_range_contains(begin + size + 8ULL, begin, size),
          "address above active kernel stack is rejected");
    check(!stack_range_contains(begin, begin, 0ULL),
          "empty kernel stack range is rejected");
    check(!stack_range_contains(0ULL, ~0ULL - 7ULL, 16ULL),
          "overflowing kernel stack range is rejected");

    check(starlix::stlcs::DISPLAY_TILE_EDGE == 32u &&
          starlix::stlcs::DISPLAY_TILE_MAX_PIXELS == 1024u,
          "HostApi and user component share a 32x32 display tile ABI");
    check(starlix::stlcs::DISPLAY_TILE_MAX_PIXELS * sizeof(stlcs_u32) <= 4096u,
          "kernel DisplayBlit scratch is bounded to 4 KiB");

    std::printf("EXCEPTION_FRAME_POLICY_PHASE2N5: %u checks, %u failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
