// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using size_t = unsigned long long;
#endif

namespace starlix::drivers::ata {

struct AtaPioDeviceInfo {
    bool present;
    uint8_t channel;
    uint8_t drive;
    uint8_t controller_bus;
    uint8_t controller_slot;
    uint8_t controller_function;
    uint16_t command_base;
    uint16_t control_port;
    bool lba48;
    uint64_t last_lba;
    uint32_t logical_sector_bytes;
};

/* Scans PCI IDE controllers and probes master/slave devices with IDENTIFY. */
bool ata_pio_init();
size_t ata_pio_device_count();
bool ata_pio_device_get(size_t index, AtaPioDeviceInfo* output);
/* Polling-only reads; no ATA DMA, write commands, or disk mutation. */
bool ata_pio_read_blocks(const AtaPioDeviceInfo& device, uint64_t lba,
                         uint16_t block_count, void* output,
                         uint32_t output_capacity);

} // namespace starlix::drivers::ata
