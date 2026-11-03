#include <cstdio>
#include <stdint.h>
#include "phys_alloc_math.hpp"

using starlix::mem::detail::phys_page_count_to_bytes;
using starlix::mem::detail::phys_reservation_end;

static int Fail(const char* message) {
    std::printf("FAIL: %s\n", message);
    return 1;
}

int main() {
    using u64 = unsigned long long;
    constexpr u64 page = 4096ULL;
    const u64 maximum = ~static_cast<u64>(0);
    u64 bytes = 0;
    u64 next = 0;

    if (phys_page_count_to_bytes(1, bytes) != true || bytes != page)
        return Fail("one-page byte conversion");

    if (!phys_page_count_to_bytes(3, bytes) || bytes != 3 * page)
        return Fail("three-page byte conversion");

    if (phys_page_count_to_bytes(0, bytes) || bytes != 0)
        return Fail("zero page count rejection");

    if (phys_page_count_to_bytes(maximum, bytes) || bytes != 0)
        return Fail("page-count multiplication overflow rejection");

    if (!phys_page_count_to_bytes(maximum / page, bytes) ||
        bytes != (maximum / page) * page)
        return Fail("largest non-overflowing page count");

    if (!phys_reservation_end(0x200000, 0x210000, 0x10000, next) ||
        next != 0x210000)
        return Fail("exact-fit contiguous allocation");

    if (!phys_reservation_end(0x200000, 0x210000, 3 * page, next) ||
        next != 0x203000)
        return Fail("three-page allocation end");

    if (phys_reservation_end(0x200000, 0x210000, 0x11000, next))
        return Fail("allocation past memory limit rejection");

    if (phys_reservation_end(0, 0x210000, page, next))
        return Fail("zero physical address rejection");

    if (phys_reservation_end(0x200001, 0x210000, page, next))
        return Fail("unaligned allocation start rejection");

    if (phys_reservation_end(0x200000, 0x210001, page, next))
        return Fail("unaligned memory limit rejection");

    if (phys_reservation_end(0x200000, 0x210000, 0, next))
        return Fail("zero-byte allocation rejection");

    if (phys_reservation_end(0x200000, 0x210000, page + 1, next))
        return Fail("unaligned allocation size rejection");

    if (phys_reservation_end(0x220000, 0x210000, page, next))
        return Fail("current address beyond limit rejection");

    // Simulate sequential non-overlapping claims from one allocator range.
    u64 current = 0x400000;
    const u64 limit = 0x410000;

    if (!phys_reservation_end(current, limit, 2 * page, next))
        return Fail("first sequential allocation");
    current = next;

    if (!phys_reservation_end(current, limit, 3 * page, next))
        return Fail("second sequential allocation");
    current = next;

    if (current != 0x405000)
        return Fail("sequential allocation arithmetic");

    std::printf("PASS: page-count conversion and overflow checks\n");
    std::printf("PASS: contiguous range alignment and boundary checks\n");
    std::printf("PASS: exact-fit and memory-exhaustion checks\n");
    std::printf("PASS: sequential non-overlapping allocation arithmetic\n");
    std::printf("NOTE: pure host arithmetic tests; physical RAM was not allocated.\n");
    return 0;
}
