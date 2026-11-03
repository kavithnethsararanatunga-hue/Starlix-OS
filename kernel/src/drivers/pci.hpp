// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../cpu/io.hpp"
#include "../../../include/driver/pci_abi.hpp"

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::drivers::pci {

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
bool pci_query_device(uint8_t bus, uint8_t slot, uint8_t func,
                      StarlixPciDeviceInfoV1& output);
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
using PciDeviceVisitorV1 = bool (*)(const StarlixPciDeviceInfoV1&, void* context);
uint32_t pci_scan_all(PciDeviceVisitorV1 visitor = nullptr, void* context = nullptr);
bool pci_find_ehci(uint8_t& bus_out, uint8_t& slot_out, uint8_t& func_out, uint32_t& bar0_out);
bool pci_find_uhci(uint8_t& bus_out, uint8_t& slot_out, uint8_t& func_out, uint32_t& bar4_out);

} // namespace starlix::drivers::pci
