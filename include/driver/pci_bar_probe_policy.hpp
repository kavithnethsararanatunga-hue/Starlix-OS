// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "pci_resource_abi.hpp"

enum StarlixPciBarProbeStatusV1 : StPciU32 {
    StarlixPciBarProbeSuccessV1 = 0u,
    StarlixPciBarProbeInvalidArgumentV1,
    StarlixPciBarProbeDeviceChangedV1,
    StarlixPciBarProbeUnsupportedBarV1,
    StarlixPciBarProbeDecodeDisableFailedV1,
    StarlixPciBarProbeReadFailedV1,
    StarlixPciBarProbeWriteFailedV1,
    StarlixPciBarProbeRestoreFailedV1,
    StarlixPciBarProbeInvalidMaskV1
};

struct StarlixPciConfigAccessV1 {
    void* context;
    bool (*read16)(void*, StPciU8, StPciU8, StPciU8, StPciU8, StPciU16*);
    bool (*write16)(void*, StPciU8, StPciU8, StPciU8, StPciU8, StPciU16);
    bool (*read32)(void*, StPciU8, StPciU8, StPciU8, StPciU8, StPciU32*);
    bool (*write32)(void*, StPciU8, StPciU8, StPciU8, StPciU8, StPciU32);
};

inline bool StarlixPciComputeBarSizeV1(
    StPciU32 mask_low, StPciU32 mask_high, bool is_64_bit,
    StPciU64 base, StPciU64* size_out) {
    if (!size_out || base == 0u) return false;
    *size_out = 0u;

    StPciU64 size = 0u;
    if (is_64_bit) {
        const StPciU64 mask =
            (static_cast<StPciU64>(mask_high) << 32u) |
            static_cast<StPciU64>(mask_low & 0xFFFFFFF0u);
        if (mask == 0u) return false;
        size = (~mask) + 1u;
    } else {
        const StPciU32 mask = mask_low & 0xFFFFFFF0u;
        if (mask == 0u) return false;
        size = static_cast<StPciU64>(
            static_cast<StPciU32>(~mask) + 1u);
    }

    if (size < 4096u || (size & (size - 1u)) != 0u ||
        (base & (size - 1u)) != 0u ||
        size > (~static_cast<StPciU64>(0) - base))
        return false;

    *size_out = size;
    return true;
}

inline StarlixPciBarProbeStatusV1 StarlixPciProbeBarResourceV1(
    const StarlixPciDeviceInfoV1* device, StPciU32 bar_index,
    const StarlixPciConfigAccessV1* access,
    StarlixPciResourceV1* output) {
    if (output) *output = StarlixPciResourceV1{};
    if (!device || !access || !output ||
        device->struct_size < sizeof(StarlixPciDeviceInfoV1) ||
        device->abi_major != STARLIX_PCI_DEVICE_ABI_MAJOR ||
        device->abi_minor > STARLIX_PCI_DEVICE_ABI_MINOR ||
        (device->flags & StarlixPciDevicePresentV1) == 0u ||
        device->device >= 32u || device->function >= 8u ||
        !access->read16 || !access->write16 ||
        !access->read32 || !access->write32 || bar_index >= STARLIX_PCI_BAR_COUNT)
        return StarlixPciBarProbeInvalidArgumentV1;

    StPciU32 bar_count = 0u;
    if (device->header_type == 0u) bar_count = 6u;
    else if (device->header_type == 1u) bar_count = 2u;
    else if (device->header_type == 2u) bar_count = 1u;
    if (bar_index >= bar_count) return StarlixPciBarProbeUnsupportedBarV1;

    const StarlixPciBarInfoV1& bar = device->bars[bar_index];
    if (!StarlixPciBarIsAssignedMemoryV1(&bar) ||
        (bar.flags & (StarlixPciBarBelow1MiBV1 |
                      StarlixPciBarReservedTypeV1 |
                      StarlixPciBarContinuationV1)) != 0u)
        return StarlixPciBarProbeUnsupportedBarV1;

    const bool is_64_bit = (bar.flags & StarlixPciBar64BitV1) != 0u;
    if (is_64_bit && bar_index + 1u >= bar_count)
        return StarlixPciBarProbeUnsupportedBarV1;

    StPciU32 vendor_device = 0u;
    StPciU32 class_header = 0u;
    StPciU32 current_low = 0u;
    StPciU32 current_high = 0u;
    if (!access->read32(access->context, device->bus, device->device,
            device->function, 0x00u, &vendor_device) ||
        !access->read32(access->context, device->bus, device->device,
            device->function, 0x0Cu, &class_header) ||
        !access->read32(access->context, device->bus, device->device,
            device->function, static_cast<StPciU8>(0x10u + bar_index * 4u),
            &current_low))
        return StarlixPciBarProbeReadFailedV1;
    if (static_cast<StPciU16>(vendor_device & 0xFFFFu) != device->vendor_id ||
        static_cast<StPciU16>(vendor_device >> 16u) != device->device_id ||
        static_cast<StPciU8>((class_header >> 16u) & 0x7Fu) != device->header_type ||
        current_low != bar.raw_low)
        return StarlixPciBarProbeDeviceChangedV1;

    const StPciU8 bar_offset =
        static_cast<StPciU8>(0x10u + bar_index * 4u);
    if (is_64_bit) {
        if (device->bars[bar_index + 1u].flags != StarlixPciBarContinuationV1 ||
            !access->read32(access->context, device->bus, device->device,
                device->function, static_cast<StPciU8>(bar_offset + 4u),
                &current_high) || current_high != bar.raw_high)
            return StarlixPciBarProbeDeviceChangedV1;
    }

    StPciU16 original_command = 0u;
    if (!access->read16(access->context, device->bus, device->device,
            device->function, 0x04u, &original_command))
        return StarlixPciBarProbeReadFailedV1;

    bool command_write_attempted = false;
    bool low_write_attempted = false;
    bool high_write_attempted = false;
    bool restore_ok = true;
    const StPciU16 decode_disabled = static_cast<StPciU16>(
        original_command & static_cast<StPciU16>(~0x0003u));
    const auto restore = [&]() {
        bool bars_ok = true;
        if (high_write_attempted)
            bars_ok = access->write32(access->context, device->bus, device->device,
                device->function, static_cast<StPciU8>(bar_offset + 4u),
                current_high) && bars_ok;
        if (low_write_attempted)
            bars_ok = access->write32(access->context, device->bus, device->device,
                device->function, bar_offset, current_low) && bars_ok;

        StPciU32 verify_low = 0u;
        StPciU32 verify_high = 0u;
        if (low_write_attempted)
            bars_ok = access->read32(access->context, device->bus, device->device,
                device->function, bar_offset, &verify_low) &&
                verify_low == current_low && bars_ok;
        if (high_write_attempted)
            bars_ok = access->read32(access->context, device->bus, device->device,
                device->function, static_cast<StPciU8>(bar_offset + 4u),
                &verify_high) && verify_high == current_high && bars_ok;

        // Never re-enable address decoding unless every BAR dword is restored
        // and read back exactly. Failed restoration leaves decode disabled.
        if (!bars_ok) return false;

        if (command_write_attempted) {
            if (!access->write16(access->context, device->bus, device->device,
                    device->function, 0x04u, original_command)) {
                (void)access->write16(access->context, device->bus, device->device,
                    device->function, 0x04u, decode_disabled);
                return false;
            }
            StPciU16 verify_command = 0u;
            if (!access->read16(access->context, device->bus, device->device,
                    device->function, 0x04u, &verify_command) ||
                verify_command != original_command) {
                (void)access->write16(access->context, device->bus, device->device,
                    device->function, 0x04u, decode_disabled);
                return false;
            }
        }
        return true;
    };

    command_write_attempted = true;
    if (!access->write16(access->context, device->bus, device->device,
            device->function, 0x04u, decode_disabled)) {
        restore_ok = restore();
        return restore_ok ? StarlixPciBarProbeDecodeDisableFailedV1
                          : StarlixPciBarProbeRestoreFailedV1;
    }
    StPciU16 command_verify = 0u;
    if (!access->read16(access->context, device->bus, device->device,
            device->function, 0x04u, &command_verify) ||
        (command_verify & 0x0003u) != 0u) {
        restore_ok = restore();
        return restore_ok ? StarlixPciBarProbeDecodeDisableFailedV1
                          : StarlixPciBarProbeRestoreFailedV1;
    }

    StPciU32 mask_low = 0u;
    StPciU32 mask_high = 0u;
    if (is_64_bit) {
        high_write_attempted = true;
        if (!access->write32(access->context, device->bus, device->device,
                device->function, static_cast<StPciU8>(bar_offset + 4u),
                0xFFFFFFFFu)) {
            restore_ok = restore();
            return restore_ok ? StarlixPciBarProbeWriteFailedV1
                              : StarlixPciBarProbeRestoreFailedV1;
        }
    }
    low_write_attempted = true;
    if (!access->write32(access->context, device->bus, device->device,
            device->function, bar_offset, 0xFFFFFFFFu) ||
        !access->read32(access->context, device->bus, device->device,
            device->function, bar_offset, &mask_low) ||
        (is_64_bit && !access->read32(access->context, device->bus,
            device->device, device->function,
            static_cast<StPciU8>(bar_offset + 4u), &mask_high))) {
        restore_ok = restore();
        return restore_ok ? StarlixPciBarProbeReadFailedV1
                          : StarlixPciBarProbeRestoreFailedV1;
    }

    restore_ok = restore();
    if (!restore_ok) return StarlixPciBarProbeRestoreFailedV1;
    if ((mask_low & 0x0Fu) != (bar.raw_low & 0x0Fu))
        return StarlixPciBarProbeInvalidMaskV1;

    StPciU64 size = 0u;
    if (!StarlixPciComputeBarSizeV1(mask_low, mask_high, is_64_bit,
            bar.base, &size))
        return StarlixPciBarProbeInvalidMaskV1;

    output->struct_size = sizeof(StarlixPciResourceV1);
    output->abi_major = STARLIX_PCI_RESOURCE_ABI_MAJOR;
    output->abi_minor = STARLIX_PCI_RESOURCE_ABI_MINOR;
    output->flags = StarlixPciResourceAssignedV1 |
        StarlixPciResourceMemoryV1 | StarlixPciResourceSizeValidatedV1;
    if (is_64_bit) output->flags |= StarlixPciResource64BitV1;
    if ((bar.flags & StarlixPciBarPrefetchableV1) != 0u)
        output->flags |= StarlixPciResourcePrefetchableV1;
    output->bus = device->bus;
    output->device = device->device;
    output->function = device->function;
    output->bar_index = static_cast<StPciU8>(bar_index);
    output->base = bar.base;
    output->size = size;
    return StarlixPciBarProbeSuccessV1;
}

struct StarlixPciResourceClaimRecordV1 {
    StPciU64 base;
    StPciU64 size;
    StPciU64 token;
    StPciU8 bus;
    StPciU8 device;
    StPciU8 function;
    StPciU8 bar_index;
    StPciU32 flags;
    StPciU32 active;
};

inline bool StarlixPciResourceRangeOverlapsV1(
    StPciU64 a_base, StPciU64 a_size,
    StPciU64 b_base, StPciU64 b_size) {
    if (!a_size || !b_size ||
        a_size > (~static_cast<StPciU64>(0) - a_base) ||
        b_size > (~static_cast<StPciU64>(0) - b_base)) return true;
    const StPciU64 a_end = a_base + a_size;
    const StPciU64 b_end = b_base + b_size;
    return a_base < b_end && b_base < a_end;
}

inline bool StarlixPciResourceRegistryClaimV1(
    StarlixPciResourceClaimRecordV1* records, StPciU32 capacity,
    StPciU64* next_token, StarlixPciResourceV1* resource) {
    if (!records || !capacity || !next_token || !resource ||
        !StarlixPciResourceRangeValidV1(resource)) return false;
    StPciU32 free_index = capacity;
    for (StPciU32 i = 0u; i < capacity; ++i) {
        if (!records[i].active && free_index == capacity) free_index = i;
        if (records[i].active &&
            StarlixPciResourceRangeOverlapsV1(resource->base, resource->size,
                records[i].base, records[i].size)) return false;
    }
    if (free_index == capacity || *next_token == 0u ||
        *next_token == ~static_cast<StPciU64>(0)) return false;

    const StPciU64 token = (*next_token)++;
    StarlixPciResourceClaimRecordV1 record{};
    record.base = resource->base;
    record.size = resource->size;
    record.token = token;
    record.bus = resource->bus;
    record.device = resource->device;
    record.function = resource->function;
    record.bar_index = resource->bar_index;
    record.flags = resource->flags;
    record.active = 1u;
    records[free_index] = record;
    resource->flags |= StarlixPciResourceClaimedV1;
    resource->ownership_token = token;
    return true;
}

inline bool StarlixPciResourceRegistryReleaseV1(
    StarlixPciResourceClaimRecordV1* records, StPciU32 capacity,
    StPciU64 token) {
    if (!records || !capacity || token == 0u) return false;
    for (StPciU32 i = 0u; i < capacity; ++i) {
        if (records[i].active && records[i].token == token) {
            records[i] = StarlixPciResourceClaimRecordV1{};
            return true;
        }
    }
    return false;
}
