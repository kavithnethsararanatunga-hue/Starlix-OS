#pragma once

// Shared freestanding-kernel and host-test arithmetic.
// Starlix defines uint64_t as unsigned long long.

namespace starlix::mem::detail {

using PhysAllocU64 = unsigned long long;

static constexpr PhysAllocU64 PHYS_PAGE_BYTES = 4096ULL;

inline bool phys_page_count_to_bytes(
    PhysAllocU64 page_count,
    PhysAllocU64& out_bytes) {
    out_bytes = 0;

    if (page_count == 0 ||
        page_count > (~static_cast<PhysAllocU64>(0) / PHYS_PAGE_BYTES))
        return false;

    out_bytes = page_count * PHYS_PAGE_BYTES;
    return out_bytes != 0;
}

inline bool phys_reservation_end(
    PhysAllocU64 current,
    PhysAllocU64 limit,
    PhysAllocU64 bytes,
    PhysAllocU64& out_next) {
    out_next = 0;

    if (current == 0 || bytes == 0 ||
        (current & (PHYS_PAGE_BYTES - 1ULL)) != 0 ||
        (limit & (PHYS_PAGE_BYTES - 1ULL)) != 0 ||
        (bytes & (PHYS_PAGE_BYTES - 1ULL)) != 0 ||
        current > limit)
        return false;

    if (bytes > limit - current)
        return false;

    out_next = current + bytes;
    return out_next > current;
}

} // namespace starlix::mem::detail
