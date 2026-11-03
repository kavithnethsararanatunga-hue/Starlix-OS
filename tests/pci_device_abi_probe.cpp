// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "driver/pci_abi.hpp"

#include <cstdio>

static bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    else std::printf("PASS: %s\n", message);
    return condition;
}

int main() {
    bool ok = true;
    StarlixPciBarInfoV1 bar{};

    ok &= check(StarlixPciDecodeBarV1(
        0xD0000008u, 0u, 0u, 6u, &bar), "32-bit BAR decode succeeds");
    ok &= check(bar.base == 0xD0000000ULL &&
        StarlixPciBarIsAssignedMemoryV1(&bar) &&
        (bar.flags & StarlixPciBarPrefetchableV1) != 0u,
        "32-bit memory BAR base and flags decoded");

    ok &= check(StarlixPciDecodeBarV1(
        0x0000000Cu, 0x00000012u, 0u, 6u, &bar),
        "64-bit BAR decode succeeds");
    ok &= check(bar.base == 0x0000001200000000ULL &&
        StarlixPciBarIsAssignedMemoryV1(&bar) &&
        (bar.flags & StarlixPciBar64BitV1) != 0u,
        "64-bit BAR combines high and low dwords");

    ok &= check(StarlixPciDecodeBarV1(
        0x0000C001u, 0u, 0u, 6u, &bar), "I/O BAR decode succeeds");
    ok &= check((bar.flags & StarlixPciBarIoSpaceV1) != 0u &&
        !StarlixPciBarIsAssignedMemoryV1(&bar),
        "I/O BAR is never treated as MMIO");

    ok &= check(StarlixPciDecodeBarV1(
        0x00020002u, 0u, 0u, 6u, &bar), "below-1MiB BAR decode succeeds");
    ok &= check((bar.flags & StarlixPciBarBelow1MiBV1) != 0u &&
        !StarlixPciBarIsAssignedMemoryV1(&bar),
        "below-1MiB BAR is rejected as normal MMIO");

    ok &= check(StarlixPciDecodeBarV1(
        0x00000006u, 0u, 0u, 6u, &bar), "reserved BAR decode reports safely");
    ok &= check((bar.flags & StarlixPciBarReservedTypeV1) != 0u &&
        !StarlixPciBarIsAssignedMemoryV1(&bar),
        "reserved memory BAR type is rejected");

    ok &= check(StarlixPciDecodeBarV1(
        0u, 0u, 0u, 6u, &bar), "unassigned BAR decode succeeds");
    ok &= check((bar.flags & StarlixPciBarUnassignedV1) != 0u &&
        !StarlixPciBarIsAssignedMemoryV1(&bar),
        "zero BAR is never treated as a resource");

    ok &= check(StarlixPciDecodeBarV1(
        0x00000004u, 0u, 5u, 6u, &bar) &&
        (bar.flags & StarlixPciBarReservedTypeV1) != 0u &&
        !StarlixPciBarIsAssignedMemoryV1(&bar),
        "64-bit BAR without a paired slot is marked unusable");
    ok &= check(!StarlixPciDecodeBarV1(
        0u, 0u, 0u, 7u, &bar), "BAR count above six is rejected");

    return ok ? 0 : 1;
}
