/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#include "../kernel/src/drivers/Gpu/Basic/Intel/intel_gpu_pci_policy.hpp"
#include <cstdio>

using namespace starlix::drivers::intel_gpu;

static bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main() {
    bool ok = true;
    ok &= Check(IsIntelDisplayController(0x8086u, 0x03u),
                "Intel display class accepted");
    ok &= Check(!IsIntelDisplayController(0x10DEu, 0x03u),
                "non-Intel display rejected");
    ok &= Check(!IsIntelDisplayController(0x8086u, 0x02u),
                "non-display Intel device rejected");
    ok &= Check(IsSupportedKextDevice(0x8086u, 0x0152u),
                "supported HD 2500 KEXT PCI ID accepted");
    ok &= Check(!IsSupportedKextDevice(0x8086u, 0x0162u),
                "other Intel PCI ID not falsely claimed");
    ok &= Check(!IsSupportedKextDevice(0x10DEu, 0x0152u),
                "same device ID from other vendor rejected");

    MemoryBarInfo bar{};
    auto status = DecodeMemoryBar0(0xF0000008u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::Valid32 &&
                bar.base == 0xF0000000ULL && !bar.is64Bit &&
                bar.prefetchable,
                "32-bit prefetchable memory BAR decoded");

    status = DecodeMemoryBar0(0xF000000Cu, 0x00000012u, bar);
    ok &= Check(status == MemoryBarStatus::Valid64 &&
                bar.base == 0x00000012F0000000ULL && bar.is64Bit &&
                bar.prefetchable,
                "64-bit prefetchable memory BAR decoded");

    status = DecodeMemoryBar0(0xF0000000u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::Valid32 &&
                bar.base == 0xF0000000ULL && !bar.prefetchable,
                "32-bit non-prefetchable memory BAR decoded");

    status = DecodeMemoryBar0(0x0000C001u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::IoSpace,
                "I/O BAR rejected for MMIO driver");
    status = DecodeMemoryBar0(0x000A0002u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::BelowOneMegabyte,
                "below-1MiB BAR rejected");
    status = DecodeMemoryBar0(0x12345606u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::ReservedType,
                "reserved memory BAR type rejected");
    status = DecodeMemoryBar0(0u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::Unassigned,
                "zero/unassigned BAR rejected");
    status = DecodeMemoryBar0(0xFFFFFFFFu, 0xFFFFFFFFu, bar);
    ok &= Check(status == MemoryBarStatus::Unassigned,
                "all-ones/unassigned BAR rejected");
    status = DecodeMemoryBar0(0x00000008u, 0u, bar);
    ok &= Check(status == MemoryBarStatus::ZeroAddress,
                "zero decoded MMIO base rejected");

    ok &= Check(IsValidMemoryBar(MemoryBarStatus::Valid32) &&
                IsValidMemoryBar(MemoryBarStatus::Valid64) &&
                !IsValidMemoryBar(MemoryBarStatus::IoSpace),
                "valid BAR status classification");

    if (!ok) return 1;
    std::puts("PASS: Intel display PCI identity policy");
    std::puts("PASS: exact KEXT target identity 8086:0152");
    std::puts("PASS: 32-bit and 64-bit BAR0 decoding");
    std::puts("PASS: I/O, below-1MiB, reserved, unassigned and zero BAR rejection");
    std::puts("NOTE: host policy tests only; no PCI hardware or MMIO accessed.");
    return 0;
}
