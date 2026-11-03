// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "../../../include/driver/pci_bar_probe_policy.hpp"
#include "pci_resource.hpp"
#include "../mem/address_space.hpp"

namespace starlix::drivers::pci {
namespace {
constexpr uint16_t PCI_CONFIG_ADDRESS = 0xCF8u;
constexpr uint16_t PCI_CONFIG_DATA = 0xCFCu;
constexpr uint32_t PCI_RESOURCE_CLAIM_CAPACITY = 32u;

static StarlixPciResourceClaimRecordV1 g_claims[PCI_RESOURCE_CLAIM_CAPACITY]{};
static uint64_t g_next_claim_token = 1u;
static bool g_config_transaction_active = false;

class PciConfigCriticalSection {
public:
    PciConfigCriticalSection() {
        asm volatile("pushfq; popq %0; cli"
                     : "=r"(saved_flags_) : : "memory");
        if (g_config_transaction_active) {
            if ((saved_flags_ & (1ULL << 9u)) != 0ULL)
                asm volatile("sti" : : : "memory");
            return;
        }
        g_config_transaction_active = true;
        active_ = true;
    }

    ~PciConfigCriticalSection() {
        if (active_) g_config_transaction_active = false;
        if ((saved_flags_ & (1ULL << 9u)) != 0ULL)
            asm volatile("sti" : : : "memory");
    }

    bool active() const { return active_; }

private:
    uint64_t saved_flags_{};
    bool active_{};
};

static uint32_t config_address(uint8_t bus, uint8_t device,
                               uint8_t function, uint8_t offset) {
    return (static_cast<uint32_t>(bus) << 16u) |
           (static_cast<uint32_t>(device) << 11u) |
           (static_cast<uint32_t>(function) << 8u) |
           (static_cast<uint32_t>(offset) & 0xFCu) | 0x80000000u;
}

static bool config_read16(void*, StPciU8 bus, StPciU8 device,
                          StPciU8 function, StPciU8 offset,
                          StPciU16* output) {
    if (!output || (offset & 1u) != 0u || offset > 0xFEu) return false;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS,
        config_address(bus, device, function, offset));
    *output = starlix::cpu::inw(static_cast<uint16_t>(
        PCI_CONFIG_DATA + (offset & 0x02u)));
    return true;
}

static bool config_write16(void*, StPciU8 bus, StPciU8 device,
                           StPciU8 function, StPciU8 offset,
                           StPciU16 value) {
    if ((offset & 1u) != 0u || offset > 0xFEu) return false;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS,
        config_address(bus, device, function, offset));
    // Use a 16-bit write for the PCI Command register. A 32-bit write at
    // 0x04 would also write Status W1C bits and could clear device status.
    starlix::cpu::outw(static_cast<uint16_t>(
        PCI_CONFIG_DATA + (offset & 0x02u)), value);
    return true;
}

static bool config_read32(void*, StPciU8 bus, StPciU8 device,
                          StPciU8 function, StPciU8 offset,
                          StPciU32* output) {
    if (!output || (offset & 3u) != 0u) return false;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS,
        config_address(bus, device, function, offset));
    *output = starlix::cpu::inl(PCI_CONFIG_DATA);
    return true;
}

static bool config_write32(void*, StPciU8 bus, StPciU8 device,
                           StPciU8 function, StPciU8 offset,
                           StPciU32 value) {
    if ((offset & 3u) != 0u) return false;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS,
        config_address(bus, device, function, offset));
    starlix::cpu::outl(PCI_CONFIG_DATA, value);
    return true;
}

static const StarlixPciConfigAccessV1 g_config_access = {
    nullptr, &config_read16, &config_write16,
    &config_read32, &config_write32
};

} // namespace

bool pci_claim_bar_resource(const StarlixPciDeviceInfoV1& device,
                            uint32_t bar_index,
                            StarlixPciResourceV1& output) {
    output = StarlixPciResourceV1{};
    if (!starlix::mem::address_space_manager_ready() ||
        starlix::mem::address_space_online_cpu_count() != 1u)
        return false;

    PciConfigCriticalSection critical;
    if (!critical.active()) return false;

    StarlixPciResourceV1 candidate{};
    const StarlixPciBarProbeStatusV1 status =
        StarlixPciProbeBarResourceV1(&device, bar_index,
            &g_config_access, &candidate);
    if (status != StarlixPciBarProbeSuccessV1) return false;

    if (!StarlixPciResourceRegistryClaimV1(g_claims,
            PCI_RESOURCE_CLAIM_CAPACITY, &g_next_claim_token, &candidate))
        return false;
    if (!StarlixPciResourceIsValidV1(&candidate)) {
        (void)StarlixPciResourceRegistryReleaseV1(g_claims,
            PCI_RESOURCE_CLAIM_CAPACITY, candidate.ownership_token);
        return false;
    }

    output = candidate;
    return true;
}

bool pci_release_bar_resource(uint64_t ownership_token) {
    PciConfigCriticalSection critical;
    if (!critical.active()) return false;
    return StarlixPciResourceRegistryReleaseV1(g_claims,
        PCI_RESOURCE_CLAIM_CAPACITY, ownership_token);
}

uint32_t pci_active_resource_claim_count() {
    PciConfigCriticalSection critical;
    if (!critical.active()) return 0u;
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < PCI_RESOURCE_CLAIM_CAPACITY; ++i)
        if (g_claims[i].active) ++count;
    return count;
}

} // namespace starlix::drivers::pci
