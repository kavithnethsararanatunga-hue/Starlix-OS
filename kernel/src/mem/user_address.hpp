// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::mem::user_address {

constexpr uint64_t PAGE_SIZE = 4096ULL;

/*
 * Reserve PML4 slot 1 (512 GiB .. 1 TiB) as Starlix's initial user region.
 * The kernel's current low identity mappings remain in PML4 slot 0. This is a
 * layout contract only; no live page-table entry is changed by this header.
 */
constexpr uint64_t USER_REGION_BASE = 0x0000008000000000ULL;
constexpr uint64_t USER_REGION_END  = 0x0000010000000000ULL; // exclusive
constexpr uint64_t USER_IMAGE_BASE  = USER_REGION_BASE + 0x00100000ULL;
constexpr uint64_t USER_STACK_TOP   = USER_REGION_END - PAGE_SIZE;
constexpr uint64_t USER_STACK_PAGES = 16ULL;
constexpr uint64_t USER_STACK_BYTES = USER_STACK_PAGES * PAGE_SIZE;
constexpr uint64_t USER_STACK_BASE  = USER_STACK_TOP - USER_STACK_BYTES;

constexpr uint64_t PTE_PRESENT  = 1ULL << 0;
constexpr uint64_t PTE_WRITABLE = 1ULL << 1;
constexpr uint64_t PTE_USER     = 1ULL << 2;
constexpr uint64_t PTE_NX       = 1ULL << 63;
constexpr uint64_t PTE_ADDRESS_MASK = 0x000FFFFFFFFFF000ULL;

constexpr uint32_t USER_PAGE_READ  = 1u << 0;
constexpr uint32_t USER_PAGE_WRITE = 1u << 1;
constexpr uint32_t USER_PAGE_EXEC  = 1u << 2;
constexpr uint32_t USER_PAGE_VALID_MASK =
    USER_PAGE_READ | USER_PAGE_WRITE | USER_PAGE_EXEC;

constexpr bool is_canonical_48(uint64_t address) {
    const uint64_t upper = address >> 48;
    const bool sign = ((address >> 47) & 1ULL) != 0;
    return sign ? upper == 0xFFFFULL : upper == 0ULL;
}

constexpr bool is_user_address(uint64_t address) {
    return is_canonical_48(address) &&
           address >= USER_REGION_BASE &&
           address < USER_REGION_END;
}

constexpr bool is_user_stack_pointer(uint64_t address) {
    if ((address & 0xFULL) != 0ULL) return false;
    if (address == USER_REGION_END) return true; // One-past-end initial stack top.
    return is_user_address(address);
}

constexpr bool is_user_range(uint64_t address, uint64_t length) {
    if (length == 0 || !is_user_address(address)) return false;
    if (length - 1ULL > (~0ULL - address)) return false;
    const uint64_t last = address + length - 1ULL;
    return is_user_address(last);
}

constexpr bool page_aligned(uint64_t value) {
    return (value & (PAGE_SIZE - 1ULL)) == 0ULL;
}

constexpr bool checked_page_span(
    uint64_t address,
    uint64_t length,
    uint64_t& out_page_base,
    uint64_t& out_page_count
) {
    out_page_base = 0;
    out_page_count = 0;
    if (length == 0 || !is_user_range(address, length)) return false;

    const uint64_t page_base = address & ~(PAGE_SIZE - 1ULL);
    const uint64_t offset = address - page_base;
    if (length > (~0ULL - offset)) return false;
    const uint64_t covered = offset + length;
    if (covered > (~0ULL - (PAGE_SIZE - 1ULL))) return false;

    const uint64_t rounded =
        (covered + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (rounded == 0) return false;

    const uint64_t page_count = rounded / PAGE_SIZE;
    const uint64_t mapped_bytes = page_count * PAGE_SIZE;
    if (mapped_bytes == 0 ||
        page_base > (~0ULL - (mapped_bytes - 1ULL)) ||
        !is_user_range(page_base, mapped_bytes)) {
        return false;
    }

    out_page_base = page_base;
    out_page_count = page_count;
    return true;
}

/*
 * Encode a leaf PTE for a user page. W^X is mandatory: a page cannot be both
 * writable and executable. NX must be supported/enabled before non-executable
 * mappings are admitted, so unsupported CPUs fail closed.
 */
constexpr bool build_user_pte(
    uint64_t physical_address,
    uint32_t permissions,
    bool nx_enabled,
    uint64_t& out_entry
) {
    out_entry = 0;
    if (!page_aligned(physical_address) ||
        (physical_address & ~PTE_ADDRESS_MASK) != 0 ||
        (permissions & ~USER_PAGE_VALID_MASK) != 0 ||
        (permissions & USER_PAGE_READ) == 0) {
        return false;
    }

    const bool writable = (permissions & USER_PAGE_WRITE) != 0;
    const bool executable = (permissions & USER_PAGE_EXEC) != 0;
    if (writable && executable) return false;
    if (!executable && !nx_enabled) return false;

    uint64_t entry = physical_address | PTE_PRESENT | PTE_USER;
    if (writable) entry |= PTE_WRITABLE;
    if (!executable) entry |= PTE_NX;
    out_entry = entry;
    return true;
}

static_assert(is_canonical_48(USER_REGION_BASE));
static_assert(is_user_address(USER_IMAGE_BASE));
static_assert(is_user_address(USER_STACK_BASE));
static_assert(!is_user_address(USER_REGION_END));
static_assert(USER_STACK_BASE < USER_STACK_TOP);
static_assert(USER_STACK_BYTES == 65536ULL);

} // namespace starlix::mem::user_address
