// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "pci.hpp"
#include "../../../include/driver/pci_resource_abi.hpp"

namespace starlix::drivers::pci {

bool pci_claim_bar_resource(const StarlixPciDeviceInfoV1& device,
                            uint32_t bar_index,
                            StarlixPciResourceV1& output);
bool pci_release_bar_resource(uint64_t ownership_token);
uint32_t pci_active_resource_claim_count();

} // namespace starlix::drivers::pci
