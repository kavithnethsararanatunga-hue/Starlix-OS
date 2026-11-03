/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once

#include "driver/pci_abi.hpp"

namespace starlix::drivers::intel_gpu {

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;


constexpr u32 kPciVendorIntel = 0x8086u;
constexpr u32 kPciClassDisplay = 0x03u;
constexpr u32 kSupportedDeviceIvyBridgeHd2500 = 0x0152u;

constexpr bool IsIntelDisplayController(u32 vendor, u32 classCode) {
    return vendor == kPciVendorIntel && classCode == kPciClassDisplay;
}

constexpr bool IsSupportedKextDevice(u32 vendor, u32 device) {
    return vendor == kPciVendorIntel &&
           device == kSupportedDeviceIvyBridgeHd2500;
}

enum class MemoryBarStatus : u8 {
    Valid32 = 0,
    Valid64 = 1,
    Unassigned = 2,
    IoSpace = 3,
    BelowOneMegabyte = 4,
    ReservedType = 5,
    ZeroAddress = 6
};

struct MemoryBarInfo {
    u64 base;
    bool is64Bit;
    bool prefetchable;
};

constexpr MemoryBarStatus DecodeMemoryBar0(
    u32 rawLow, u32 rawHigh, MemoryBarInfo& output) {
    output = MemoryBarInfo{0, false, false};

    if (rawLow == 0u || rawLow == 0xFFFFFFFFu)
        return MemoryBarStatus::Unassigned;

    if ((rawLow & 1u) != 0u)
        return MemoryBarStatus::IoSpace;

    const u32 memoryType = (rawLow >> 1u) & 0x3u;
    if (memoryType == 0x1u)
        return MemoryBarStatus::BelowOneMegabyte;
    if (memoryType == 0x3u)
        return MemoryBarStatus::ReservedType;

    const bool is64Bit = memoryType == 0x2u;
    const u64 lowAddress = static_cast<u64>(rawLow & 0xFFFFFFF0u);
    const u64 base = is64Bit
        ? (static_cast<u64>(rawHigh) << 32u) | lowAddress
        : lowAddress;

    if (base == 0u)
        return MemoryBarStatus::ZeroAddress;

    output.base = base;
    output.is64Bit = is64Bit;
    output.prefetchable = (rawLow & 0x8u) != 0u;
    return is64Bit ? MemoryBarStatus::Valid64 : MemoryBarStatus::Valid32;
}

constexpr bool IsValidMemoryBar(MemoryBarStatus status) {
    return status == MemoryBarStatus::Valid32 ||
           status == MemoryBarStatus::Valid64;
}

} // namespace starlix::drivers::intel_gpu

// The descriptor is produced by the generic kernel PCI subsystem. This
// policy is compiled into the external KEXT and performs no PCI I/O/MMIO.
namespace starlix::drivers::intel_gpu {
inline bool IsSupportedPciDevice(const StarlixPciDeviceInfoV1* device) {
    if (!device ||
        device->struct_size < sizeof(StarlixPciDeviceInfoV1) ||
        device->abi_major != STARLIX_PCI_DEVICE_ABI_MAJOR ||
        device->abi_minor > STARLIX_PCI_DEVICE_ABI_MINOR ||
        (device->flags & StarlixPciDevicePresentV1) == 0u)
        return false;

    if (!IsSupportedKextDevice(device->vendor_id, device->device_id) ||
        device->class_code != kPciClassDisplay ||
        device->subclass != 0x00u)
        return false;

    return StarlixPciBarIsAssignedMemoryV1(&device->bars[0]);
}
} // namespace starlix::drivers::intel_gpu
