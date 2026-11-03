// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
#endif

namespace starlix::drivers::ata::policy {

struct ChannelPorts {
    uint16_t command_base;
    uint16_t control_port;
};

constexpr bool io_bar_base(uint32_t raw, uint16_t control_offset,
                           uint16_t* output) {
    if (!output || (raw & 1u) == 0u || raw == 0xFFFFFFFFu) return false;
    const uint32_t base = raw & 0xFFFFFFFCu;
    const uint32_t decoded = base + control_offset;
    if (base == 0u || base > 0xFFF8u || decoded == 0u || decoded > 0xFFFCu)
        return false;
    *output = static_cast<uint16_t>(decoded);
    return true;
}

constexpr bool channel_ports(uint8_t programming_interface, uint8_t channel,
                             uint32_t command_bar, uint32_t control_bar,
                             ChannelPorts* output) {
    if (!output || channel > 1u) return false;
    const uint8_t native_bit = channel == 0u ? 0x01u : 0x04u;
    const uint8_t programmable_bit = channel == 0u ? 0x02u : 0x08u;
    if ((programming_interface & native_bit) == 0u) {
        output->command_base = channel == 0u ? 0x1F0u : 0x170u;
        output->control_port = channel == 0u ? 0x3F6u : 0x376u;
        return true;
    }
    if ((programming_interface & programmable_bit) == 0u) return false;
    uint16_t command = 0;
    uint16_t control_bar_base_plus_two = 0;
    if (!io_bar_base(command_bar, 0u, &command) ||
        !io_bar_base(control_bar, 2u, &control_bar_base_plus_two)) return false;
    output->command_base = command;
    output->control_port = control_bar_base_plus_two;
    return true;
}

struct IdentifyInfo {
    bool lba28;
    bool lba48;
    uint64_t last_lba;
    uint32_t logical_sector_bytes;
};

constexpr bool parse_identify(const uint16_t words[256], IdentifyInfo* output) {
    if (!words || !output) return false;
    *output = {};
    const bool lba_supported = (words[49] & (1u << 9u)) != 0u;
    if (!lba_supported) return false;

    const uint32_t logical_words =
        static_cast<uint32_t>(words[117]) |
        (static_cast<uint32_t>(words[118]) << 16u);
    uint32_t sector_bytes = 512u;
    const uint16_t sector_info = words[106];
    const bool sector_info_valid = (sector_info & 0xC000u) == 0x4000u;
    if (sector_info_valid && (sector_info & (1u << 12u)) != 0u) {
        if (logical_words < 256u || logical_words > 2048u) return false;
        sector_bytes = logical_words * 2u;
        if ((sector_bytes & (sector_bytes - 1u)) != 0u ||
            sector_bytes < 512u || sector_bytes > 4096u) return false;
    }

    const bool lba48_validity = (words[83] & 0xC000u) == 0x4000u;
    const bool lba48_supported = lba48_validity && (words[83] & (1u << 10u)) != 0u;
    uint64_t capacity = 0u;
    if (lba48_supported) {
        capacity = static_cast<uint64_t>(words[100]) |
                   (static_cast<uint64_t>(words[101]) << 16u) |
                   (static_cast<uint64_t>(words[102]) << 32u) |
                   (static_cast<uint64_t>(words[103]) << 48u);
        if (capacity == 0u || capacity > (1ull << 48u)) return false;
        output->lba48 = true;
    } else {
        capacity = static_cast<uint64_t>(words[60]) |
                   (static_cast<uint64_t>(words[61]) << 16u);
        if (capacity == 0u || capacity > (1ull << 28u)) return false;
        output->lba28 = true;
    }

    output->last_lba = capacity - 1u;
    output->logical_sector_bytes = sector_bytes;
    return true;
}

constexpr bool read_request_valid(bool present, uint64_t last_lba,
                                  uint32_t sector_bytes, uint64_t lba,
                                  uint16_t count, uint32_t capacity) {
    if (!present || count == 0u || sector_bytes < 512u ||
        sector_bytes > 4096u || (sector_bytes & (sector_bytes - 1u)) != 0u)
        return false;
    const uint64_t bytes = static_cast<uint64_t>(count) * sector_bytes;
    if (bytes > 4096u || bytes > capacity) return false;
    if (lba > last_lba || static_cast<uint64_t>(count - 1u) > last_lba - lba)
        return false;
    return true;
}

constexpr bool self_test() {
    ChannelPorts ports{};
    if (!channel_ports(0x8Au, 0u, 0u, 0u, &ports) ||
        ports.command_base != 0x1F0u || ports.control_port != 0x3F6u) return false;
    if (!channel_ports(0x8Au, 1u, 0u, 0u, &ports) ||
        ports.command_base != 0x170u || ports.control_port != 0x376u) return false;
    if (!channel_ports(0x03u, 0u, 0x000001F1u, 0x000003F1u, &ports) ||
        ports.command_base != 0x1F0u || ports.control_port != 0x3F2u) return false;
    if (channel_ports(0x01u, 0u, 0x000001F0u, 0x000003F1u, &ports)) return false;
    if (io_bar_base(0x000001F0u, 0u, nullptr)) return false;

    uint16_t identify[256]{};
    identify[49] = 1u << 9u;
    identify[60] = 0x1234u;
    identify[61] = 0u;
    IdentifyInfo info{};
    if (!parse_identify(identify, &info) || !info.lba28 || info.last_lba != 0x1233u ||
        info.logical_sector_bytes != 512u) return false;
    identify[83] = 0x4400u;
    identify[100] = 0x5678u;
    identify[101] = 0x1234u;
    identify[102] = 1u;
    identify[103] = 0u;
    if (!parse_identify(identify, &info) || !info.lba48 ||
        info.last_lba != 0x000112345677ull) return false;
    if (!read_request_valid(true, 99u, 512u, 90u, 8u, 4096u)) return false;
    if (read_request_valid(true, 99u, 512u, 90u, 10u, 4096u)) return false;
    if (read_request_valid(true, 99u, 512u, 99u, 2u, 1024u)) return false;
    if (read_request_valid(true, 99u, 512u, 0u, 1u, 511u)) return false;
    if (read_request_valid(true, ~static_cast<uint64_t>(0), 512u,
                           ~static_cast<uint64_t>(0), 2u, 1024u)) return false;
    identify[103] = 1u;
    if (parse_identify(identify, &info)) return false; /* beyond 48-bit LBA */
    return true;
}

} // namespace starlix::drivers::ata::policy
