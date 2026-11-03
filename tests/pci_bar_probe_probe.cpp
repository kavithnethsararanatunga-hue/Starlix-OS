// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "driver/pci_bar_probe_policy.hpp"

#include <cstdio>

struct MockPciConfig {
    StPciU16 command{0x0007u};
    StPciU32 vendor_device{0x01528086u};
    StPciU32 header{0u};
    StPciU32 bar_low{0xF0000008u};
    StPciU32 bar_high{0u};
    StPciU32 mask_low{0xFFF00008u};
    StPciU32 mask_high{0u};
    bool probing_low{};
    bool probing_high{};
    bool fail_mask_read{};
    bool fail_restore_low{};
    StPciU32 command_status_dword_writes{};
    StPciU32 command_disable_writes{};
    StPciU32 bar_probe_writes{};
    StPciU32 bar_restore_writes{};
};

static bool read16(void* opaque, StPciU8, StPciU8, StPciU8,
                   StPciU8 offset, StPciU16* value) {
    auto* config = static_cast<MockPciConfig*>(opaque);
    if (!config || !value || offset != 0x04u) return false;
    *value = config->command;
    return true;
}

static bool write16(void* opaque, StPciU8, StPciU8, StPciU8,
                    StPciU8 offset, StPciU16 value) {
    auto* config = static_cast<MockPciConfig*>(opaque);
    if (!config || offset != 0x04u) return false;
    if ((value & 0x0003u) == 0u) ++config->command_disable_writes;
    config->command = value;
    return true;
}

static bool read32(void* opaque, StPciU8, StPciU8, StPciU8,
                   StPciU8 offset, StPciU32* value) {
    auto* config = static_cast<MockPciConfig*>(opaque);
    if (!config || !value) return false;
    if (offset == 0x00u) *value = config->vendor_device;
    else if (offset == 0x0Cu) *value = config->header;
    else if (offset == 0x10u) {
        if (config->fail_mask_read && config->probing_low) return false;
        *value = config->probing_low ? config->mask_low : config->bar_low;
    } else if (offset == 0x14u) {
        *value = config->probing_high ? config->mask_high : config->bar_high;
    } else return false;
    return true;
}

static bool write32(void* opaque, StPciU8, StPciU8, StPciU8,
                    StPciU8 offset, StPciU32 value) {
    auto* config = static_cast<MockPciConfig*>(opaque);
    if (!config) return false;
    if (offset == 0x04u) {
        ++config->command_status_dword_writes;
        return false;
    }
    if (offset == 0x10u) {
        if (value == 0xFFFFFFFFu) {
            config->probing_low = true;
            ++config->bar_probe_writes;
            return true;
        }
        if (config->fail_restore_low) return false;
        config->bar_low = value;
        config->probing_low = false;
        ++config->bar_restore_writes;
        return true;
    }
    if (offset == 0x14u) {
        if (value == 0xFFFFFFFFu) {
            config->probing_high = true;
            ++config->bar_probe_writes;
            return true;
        }
        config->bar_high = value;
        config->probing_high = false;
        ++config->bar_restore_writes;
        return true;
    }
    return false;
}

static StarlixPciDeviceInfoV1 make_device(bool is64 = false) {
    StarlixPciDeviceInfoV1 device{};
    device.struct_size = sizeof(device);
    device.abi_major = STARLIX_PCI_DEVICE_ABI_MAJOR;
    device.abi_minor = STARLIX_PCI_DEVICE_ABI_MINOR;
    device.bus = 0u;
    device.device = 2u;
    device.function = 0u;
    device.header_type = 0u;
    device.vendor_id = 0x8086u;
    device.device_id = 0x0152u;
    device.class_code = 0x03u;
    device.subclass = 0x00u;
    device.flags = StarlixPciDevicePresentV1 |
                   StarlixPciDeviceMemoryDecodeEnabledV1;
    device.bars[0].base = is64 ? 0x00000012F0000000ULL : 0xF0000000ULL;
    device.bars[0].raw_low = is64 ? 0xF000000Cu : 0xF0000008u;
    device.bars[0].raw_high = is64 ? 0x00000012u : 0u;
    device.bars[0].flags = StarlixPciBarAssignedV1 |
        StarlixPciBarMemorySpaceV1 | StarlixPciBarPrefetchableV1 |
        (is64 ? StarlixPciBar64BitV1 : 0u);
    if (is64) {
        device.bars[1].raw_low = 0x00000012u;
        device.bars[1].flags = StarlixPciBarContinuationV1;
    }
    return device;
}

static bool check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS:" : "FAIL:", message);
    return value;
}

int main() {
    bool ok = true;
    const StarlixPciConfigAccessV1 access{
        nullptr, &read16, &write16, &read32, &write32
    };

    auto device32 = make_device(false);
    MockPciConfig config32{};
    config32.mask_low = 0xFFF00008u; // 1 MiB BAR
    StarlixPciConfigAccessV1 access32 = access;
    access32.context = &config32;
    StarlixPciResourceV1 resource32{};
    const auto result32 = StarlixPciProbeBarResourceV1(
        &device32, 0u, &access32, &resource32);
    ok &= check(result32 == StarlixPciBarProbeSuccessV1 &&
                    resource32.base == 0xF0000000ULL &&
                    resource32.size == 0x00100000ULL,
                "32-bit BAR sizing derives the exact 1 MiB range");
    ok &= check(config32.command == 0x0007u &&
                    config32.bar_low == 0xF0000008u &&
                    config32.command_disable_writes == 1u &&
                    config32.bar_probe_writes == 1u &&
                    config32.bar_restore_writes == 1u,
                "32-bit BAR and PCI Command register restore exactly");
    ok &= check(config32.command_status_dword_writes == 0u,
                "BAR sizing never performs a 32-bit Command/Status write");

    auto device64 = make_device(true);
    MockPciConfig config64{};
    config64.bar_low = 0xF000000Cu;
    config64.bar_high = 0x00000012u;
    config64.mask_low = 0xFF00000Cu;
    config64.mask_high = 0xFFFFFFFFu;
    StarlixPciConfigAccessV1 access64 = access;
    access64.context = &config64;
    StarlixPciResourceV1 resource64{};
    const auto result64 = StarlixPciProbeBarResourceV1(
        &device64, 0u, &access64, &resource64);
    ok &= check(result64 == StarlixPciBarProbeSuccessV1 &&
                    resource64.base == 0x00000012F0000000ULL &&
                    resource64.size == 0x01000000ULL &&
                    (resource64.flags & StarlixPciResource64BitV1) != 0u,
                "64-bit BAR sizing combines high/low masks into 16 MiB");
    ok &= check(config64.command == 0x0007u &&
                    config64.bar_low == 0xF000000Cu &&
                    config64.bar_high == 0x00000012u &&
                    config64.bar_probe_writes == 2u &&
                    config64.bar_restore_writes == 2u,
                "64-bit BAR pair and PCI Command register restore exactly");

    auto invalid_mask_device = make_device(false);
    MockPciConfig invalid_mask_config{};
    invalid_mask_config.mask_low = 0xFFF00000u; // BAR attributes changed by probe.
    StarlixPciConfigAccessV1 invalid_mask_access = access;
    invalid_mask_access.context = &invalid_mask_config;
    StarlixPciResourceV1 invalid_mask_resource{};
    ok &= check(StarlixPciProbeBarResourceV1(&invalid_mask_device, 0u,
                    &invalid_mask_access, &invalid_mask_resource) ==
                    StarlixPciBarProbeInvalidMaskV1 &&
                    invalid_mask_config.command == 0x0007u &&
                    invalid_mask_config.bar_low == 0xF0000008u,
                "BAR attribute mismatch fails closed after exact restoration");

    auto stale = make_device(false);
    MockPciConfig stale_config{};
    stale_config.bar_low = 0xE0000008u;
    StarlixPciConfigAccessV1 stale_access = access;
    stale_access.context = &stale_config;
    StarlixPciResourceV1 stale_resource{};
    ok &= check(StarlixPciProbeBarResourceV1(&stale, 0u, &stale_access,
                    &stale_resource) == StarlixPciBarProbeDeviceChangedV1 &&
                    stale_config.bar_probe_writes == 0u,
                "stale BDF/BAR snapshot is rejected before configuration writes");

    auto failed = make_device(false);
    MockPciConfig failed_config{};
    failed_config.fail_mask_read = true;
    StarlixPciConfigAccessV1 failed_access = access;
    failed_access.context = &failed_config;
    StarlixPciResourceV1 failed_resource{};
    ok &= check(StarlixPciProbeBarResourceV1(&failed, 0u, &failed_access,
                    &failed_resource) == StarlixPciBarProbeReadFailedV1 &&
                    failed_config.command == 0x0007u &&
                    failed_config.bar_low == 0xF0000008u,
                "read failure rolls BAR and Command state back");

    auto restore_failure = make_device(false);
    MockPciConfig restore_config{};
    restore_config.fail_restore_low = true;
    StarlixPciConfigAccessV1 restore_access = access;
    restore_access.context = &restore_config;
    StarlixPciResourceV1 restore_resource{};
    ok &= check(StarlixPciProbeBarResourceV1(&restore_failure, 0u,
                    &restore_access, &restore_resource) ==
                    StarlixPciBarProbeRestoreFailedV1 &&
                    (restore_config.command & 0x0003u) == 0u,
                "failed BAR restoration is reported and decoders remain disabled");

    StarlixPciResourceClaimRecordV1 records[4]{};
    StPciU64 next_token = 1u;
    StarlixPciResourceV1 claim_a = resource32;
    ok &= check(StarlixPciResourceRegistryClaimV1(records, 4u,
                    &next_token, &claim_a) &&
                    StarlixPciResourceIsValidV1(&claim_a) &&
                    claim_a.ownership_token == 1u,
                "resource registry grants an exclusive ownership token");
    StarlixPciResourceV1 overlap = resource32;
    overlap.base += 0x80000u;
    ok &= check(!StarlixPciResourceRegistryClaimV1(records, 4u,
                    &next_token, &overlap),
                "overlapping PCI resource claims are rejected");
    StarlixPciResourceV1 adjacent = resource32;
    adjacent.base += adjacent.size;
    ok &= check(StarlixPciResourceRegistryClaimV1(records, 4u,
                    &next_token, &adjacent),
                "non-overlapping adjacent resource claims are accepted");
    ok &= check(StarlixPciResourceRegistryReleaseV1(records, 4u,
                    claim_a.ownership_token) &&
                    !StarlixPciResourceRegistryReleaseV1(records, 4u,
                        claim_a.ownership_token),
                "resource release retires the token exactly once");

    StPciU64 invalid_size = 0u;
    ok &= check(!StarlixPciComputeBarSizeV1(0xFFFFF008u, 0u, false,
                    0xF0001001ULL, &invalid_size),
                "misaligned BAR base is rejected against the decoded aperture size");

    std::printf("PCI_BAR_PROBE_HOST: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
