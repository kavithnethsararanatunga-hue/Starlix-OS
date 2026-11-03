/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once


namespace starlix::drivers::ehci::detail {

/* This implementation currently supports only 32-bit, non-prefetchable MMIO
   BARs. Refuse I/O, 64-bit, prefetchable, zero, and mismatched BAR values. */
constexpr bool decode_ehci_mmio_bar32(unsigned int raw_bar, unsigned int discovered_bar,
                                      unsigned int* base_out) {
    if (!base_out) return false;
    if ((raw_bar & 0x1u) != 0u) return false;       // I/O BAR
    if ((raw_bar & 0x6u) != 0u) return false;       // not a 32-bit memory BAR
    if ((raw_bar & 0x8u) != 0u) return false;       // prefetchable: not accepted
    const unsigned int base = raw_bar & 0xFFFFFFF0u;
    if (base == 0u || base == 0xFFFFFFF0u) return false;
    if ((base & 0x3u) != 0u || discovered_bar != base) return false;
    *base_out = base;
    return true;
}

/* Check that [base + offset, base + offset + bytes) is representable without
   32-bit wraparound before forming the operational-register pointer. */
constexpr bool ehci_mmio_window_fits32(unsigned int base, unsigned int offset,
                                       unsigned int bytes) {
    if (base == 0u || bytes == 0u) return false;
    if (offset > 0xFFFFFFFFu - base) return false;
    const unsigned int start = base + offset;
    return (bytes - 1u) <= (0xFFFFFFFFu - start);
}

} // namespace starlix::drivers::ehci::detail
