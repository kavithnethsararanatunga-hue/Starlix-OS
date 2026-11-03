// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

typedef unsigned char StPciU8;
typedef unsigned short StPciU16;
typedef unsigned int StPciU32;
typedef unsigned long long StPciU64;

constexpr StPciU16 STARLIX_PCI_DEVICE_ABI_MAJOR = 1u;
constexpr StPciU16 STARLIX_PCI_DEVICE_ABI_MINOR = 0u;
constexpr StPciU32 STARLIX_PCI_BAR_COUNT = 6u;

enum StarlixPciDeviceFlagsV1 : StPciU32 {
    StarlixPciDevicePresentV1 = 1u << 0,
    StarlixPciDeviceMultifunctionV1 = 1u << 1,
    StarlixPciDeviceMemoryDecodeEnabledV1 = 1u << 2,
    StarlixPciDeviceBusMasterEnabledV1 = 1u << 3
};

enum StarlixPciBarFlagsV1 : StPciU32 {
    StarlixPciBarAssignedV1 = 1u << 0,
    StarlixPciBarIoSpaceV1 = 1u << 1,
    StarlixPciBarMemorySpaceV1 = 1u << 2,
    StarlixPciBar64BitV1 = 1u << 3,
    StarlixPciBarPrefetchableV1 = 1u << 4,
    StarlixPciBarBelow1MiBV1 = 1u << 5,
    StarlixPciBarReservedTypeV1 = 1u << 6,
    StarlixPciBarUnassignedV1 = 1u << 7,
    StarlixPciBarContinuationV1 = 1u << 8
};

struct StarlixPciBarInfoV1 {
    StPciU64 base;
    StPciU32 raw_low;
    StPciU32 raw_high;
    StPciU32 flags;
    StPciU32 reserved;
};

struct StarlixPciDeviceInfoV1 {
    StPciU32 struct_size;
    StPciU16 abi_major;
    StPciU16 abi_minor;

    StPciU8 bus;
    StPciU8 device;
    StPciU8 function;
    StPciU8 header_type;

    StPciU16 vendor_id;
    StPciU16 device_id;
    StPciU16 command;
    StPciU16 status;

    StPciU8 class_code;
    StPciU8 subclass;
    StPciU8 programming_interface;
    StPciU8 revision_id;

    StPciU32 flags;
    StPciU32 reserved;

    StarlixPciBarInfoV1 bars[STARLIX_PCI_BAR_COUNT];
};

// Pure decoder for a single PCI BAR. It never performs PCI configuration I/O.
// bar_count is determined from the standard PCI header type by the kernel.
inline bool StarlixPciDecodeBarV1(
    StPciU32 raw_low, StPciU32 raw_high,
    StPciU32 bar_index, StPciU32 bar_count,
    StarlixPciBarInfoV1* output) {
    if (!output || bar_index >= STARLIX_PCI_BAR_COUNT ||
        bar_count > STARLIX_PCI_BAR_COUNT || bar_index >= bar_count)
        return false;

    *output = StarlixPciBarInfoV1{};
    output->raw_low = raw_low;
    output->raw_high = raw_high;

    if (raw_low == 0u || raw_low == 0xFFFFFFFFu) {
        output->flags = StarlixPciBarUnassignedV1;
        return true;
    }

    if ((raw_low & 1u) != 0u) {
        const StPciU32 io_base = raw_low & 0xFFFFFFFCu;
        output->base = io_base;
        output->flags = StarlixPciBarIoSpaceV1;
        if (io_base != 0u) output->flags |= StarlixPciBarAssignedV1;
        else output->flags |= StarlixPciBarUnassignedV1;
        return true;
    }

    output->flags = StarlixPciBarMemorySpaceV1;
    const StPciU32 memory_type = (raw_low >> 1u) & 0x3u;
    if ((raw_low & 0x8u) != 0u)
        output->flags |= StarlixPciBarPrefetchableV1;

    if (memory_type == 0x1u) {
        output->flags |= StarlixPciBarBelow1MiBV1;
        output->base = raw_low & 0x000FFFF0u;
    } else if (memory_type == 0x2u) {
        if (bar_index + 1u >= bar_count) {
            output->flags |= StarlixPciBarReservedTypeV1;
            return true;
        }
        output->flags |= StarlixPciBar64BitV1;
        output->base = (static_cast<StPciU64>(raw_high) << 32u) |
                       static_cast<StPciU64>(raw_low & 0xFFFFFFF0u);
    } else if (memory_type == 0x3u) {
        output->flags |= StarlixPciBarReservedTypeV1;
        return true;
    } else {
        output->base = raw_low & 0xFFFFFFF0u;
    }

    if (output->base != 0u)
        output->flags |= StarlixPciBarAssignedV1;
    else
        output->flags |= StarlixPciBarUnassignedV1;
    return true;
}

inline bool StarlixPciBarIsAssignedMemoryV1(
    const StarlixPciBarInfoV1* bar) {
    if (!bar) return false;
    const StPciU32 required =
        StarlixPciBarAssignedV1 | StarlixPciBarMemorySpaceV1;
    const StPciU32 forbidden =
        StarlixPciBarIoSpaceV1 | StarlixPciBarBelow1MiBV1 |
        StarlixPciBarReservedTypeV1 | StarlixPciBarUnassignedV1 |
        StarlixPciBarContinuationV1;
    return (bar->flags & required) == required &&
           (bar->flags & forbidden) == 0u && bar->base != 0u;
}

static_assert(sizeof(StarlixPciBarInfoV1) == 24u,
              "PCI BAR ABI v1 size mismatch");
static_assert(sizeof(StarlixPciDeviceInfoV1) == 176u,
              "PCI device ABI v1 size mismatch");
