// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "ata_pio.hpp"
#include "ata_pio_policy.hpp"
#include "pci.hpp"
#include "serial.hpp"
#include "../cpu/io.hpp"

namespace starlix::drivers::ata {
namespace {

constexpr size_t kMaximumAtaDevices = 8u;
constexpr uint32_t kStatusPollLimit = 1000000u;
constexpr uint8_t kStatusErr = 0x01u;
constexpr uint8_t kStatusDrq = 0x08u;
constexpr uint8_t kStatusDf = 0x20u;
constexpr uint8_t kStatusBsy = 0x80u;
constexpr uint8_t kCommandIdentify = 0xECu;
constexpr uint8_t kCommandReadSectors = 0x20u;
constexpr uint8_t kCommandReadSectorsExt = 0x24u;
constexpr uint8_t kDeviceControlNien = 0x02u;

void print_dec(uint64_t value) {
    char buffer[24];
    uint32_t used = 0u;
    do { buffer[used++] = static_cast<char>('0' + (value % 10u)); value /= 10u; }
    while (value != 0u && used < sizeof(buffer));
    char text[24];
    uint32_t out = 0u;
    while (used != 0u) text[out++] = buffer[--used];
    text[out] = '\0';
    starlix::drivers::serial::print(text);
}

struct AtaChannel {
    uint16_t command_base;
    uint16_t control_port;
    bool valid;
};

AtaPioDeviceInfo g_devices[kMaximumAtaDevices]{};
size_t g_device_count = 0u;

void delay_400ns(uint16_t control_port) {
    (void)starlix::cpu::inb(control_port);
    (void)starlix::cpu::inb(control_port);
    (void)starlix::cpu::inb(control_port);
    (void)starlix::cpu::inb(control_port);
}

bool wait_status(const AtaChannel& channel, bool require_drq, uint8_t* final_status) {
    if (!channel.valid) return false;
    const uint16_t status_port = static_cast<uint16_t>(channel.command_base + 7u);
    for (uint32_t spin = 0u; spin < kStatusPollLimit; ++spin) {
        const uint8_t status = starlix::cpu::inb(status_port);
        if (status == 0u || status == 0xFFu) return false;
        if ((status & kStatusBsy) != 0u) continue;
        if ((status & (kStatusErr | kStatusDf)) != 0u) {
            if (final_status) *final_status = status;
            return false;
        }
        if (!require_drq || (status & kStatusDrq) != 0u) {
            if (final_status) *final_status = status;
            return true;
        }
    }
    return false;
}

bool identify_device(uint8_t channel_index, uint8_t drive,
                     uint8_t bus, uint8_t slot, uint8_t function,
                     const AtaChannel& channel, AtaPioDeviceInfo* output) {
    if (!output || channel_index > 1u || drive > 1u || !channel.valid) return false;

    const uint16_t command = channel.command_base;
    starlix::cpu::outb(channel.control_port, kDeviceControlNien);
    starlix::cpu::outb(static_cast<uint16_t>(command + 6u),
                       static_cast<uint8_t>(0xA0u | (drive << 4u)));
    delay_400ns(channel.control_port);

    /* A zero/0xFF status means the selected device is absent. */
    const uint8_t pre_status = starlix::cpu::inb(static_cast<uint16_t>(command + 7u));
    if (pre_status == 0u || pre_status == 0xFFu ||
        !wait_status(channel, false, nullptr)) return false;

    starlix::cpu::outb(static_cast<uint16_t>(command + 2u), 0u);
    starlix::cpu::outb(static_cast<uint16_t>(command + 3u), 0u);
    starlix::cpu::outb(static_cast<uint16_t>(command + 4u), 0u);
    starlix::cpu::outb(static_cast<uint16_t>(command + 5u), 0u);
    starlix::cpu::outb(static_cast<uint16_t>(command + 7u), kCommandIdentify);

    uint8_t status = 0u;
    if (!wait_status(channel, true, &status)) return false;

    uint16_t words[256]{};
    for (uint32_t i = 0u; i < 256u; ++i)
        words[i] = starlix::cpu::inw(command);
    if (!wait_status(channel, false, &status)) return false;

    policy::IdentifyInfo parsed{};
    if (!policy::parse_identify(words, &parsed)) return false;
    output->present = true;
    output->channel = channel_index;
    output->drive = drive;
    output->controller_bus = bus;
    output->controller_slot = slot;
    output->controller_function = function;
    output->command_base = channel.command_base;
    output->control_port = channel.control_port;
    output->lba48 = parsed.lba48;
    output->last_lba = parsed.last_lba;
    output->logical_sector_bytes = parsed.logical_sector_bytes;
    return true;
}

bool decode_controller_channel(uint8_t bus, uint8_t slot, uint8_t function,
                               uint8_t programming_interface, uint8_t channel_index,
                               AtaChannel* output) {
    if (!output || channel_index > 1u) return false;
    const uint32_t command_bar = starlix::drivers::pci::pci_read(
        bus, slot, function, channel_index == 0u ? 0x10u : 0x18u);
    const uint32_t control_bar = starlix::drivers::pci::pci_read(
        bus, slot, function, channel_index == 0u ? 0x14u : 0x1Cu);
    policy::ChannelPorts decoded{};
    if (!policy::channel_ports(programming_interface, channel_index,
                               command_bar, control_bar, &decoded)) return false;
    output->command_base = decoded.command_base;
    output->control_port = decoded.control_port;
    output->valid = true;
    return true;
}

void append_controller_devices(uint8_t bus, uint8_t slot, uint8_t function,
                               uint8_t programming_interface) {
    for (uint8_t channel_index = 0u; channel_index < 2u; ++channel_index) {
        AtaChannel channel{};
        if (!decode_controller_channel(bus, slot, function,
                                       programming_interface, channel_index,
                                       &channel)) continue;
        /* Do not probe an I/O range unless PCI I/O decoding is already enabled. */
        const uint32_t command_register = starlix::drivers::pci::pci_read(
            bus, slot, function, 0x04u);
        if ((command_register & 0x0001u) == 0u) {
            starlix::drivers::serial::print("[ATA] PCI_IO_DECODE=DISABLED; channel skipped\n");
            continue;
        }
        for (uint8_t drive = 0u; drive < 2u; ++drive) {
            if (g_device_count >= kMaximumAtaDevices) return;
            AtaPioDeviceInfo info{};
            if (!identify_device(channel_index, drive, bus, slot, function,
                                 channel, &info)) continue;
            bool duplicate = false;
            for (size_t existing = 0u; existing < g_device_count; ++existing) {
                const AtaPioDeviceInfo& prior = g_devices[existing];
                if (prior.command_base == info.command_base &&
                    prior.control_port == info.control_port && prior.drive == info.drive)
                    duplicate = true;
            }
            if (!duplicate) g_devices[g_device_count++] = info;
        }
    }
}

} // namespace

bool ata_pio_init() {
    g_device_count = 0u;
    for (size_t i = 0u; i < kMaximumAtaDevices; ++i) g_devices[i] = {};
    uint32_t controller_count = 0u;

    for (uint32_t bus = 0u; bus < 256u; ++bus) {
        for (uint32_t slot = 0u; slot < 32u; ++slot) {
            for (uint32_t function = 0u; function < 8u; ++function) {
                const uint32_t vendor_device = starlix::drivers::pci::pci_read(
                    static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                    static_cast<uint8_t>(function), 0x00u);
                const uint16_t vendor = static_cast<uint16_t>(vendor_device);
                if (vendor == 0u || vendor == 0xFFFFu) continue;
                const uint32_t class_revision = starlix::drivers::pci::pci_read(
                    static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                    static_cast<uint8_t>(function), 0x08u);
                const uint8_t class_code = static_cast<uint8_t>(class_revision >> 24u);
                const uint8_t subclass = static_cast<uint8_t>(class_revision >> 16u);
                const uint8_t programming_interface = static_cast<uint8_t>(class_revision >> 8u);
                if (class_code != 0x01u || subclass != 0x01u) continue;
                ++controller_count;
                starlix::drivers::serial::print("[ATA] PCI IDE controller=FOUND; BDF ");
                print_dec(bus); starlix::drivers::serial::print(":");
                print_dec(slot); starlix::drivers::serial::print(".");
                print_dec(function); starlix::drivers::serial::print(" ProgIF=0x");
                const char hex[] = "0123456789ABCDEF";
                char prog_if_text[3] = {hex[(programming_interface >> 4u) & 0x0Fu],
                                        hex[programming_interface & 0x0Fu], '\0'};
                starlix::drivers::serial::print(prog_if_text);
                starlix::drivers::serial::print("\n");
                append_controller_devices(static_cast<uint8_t>(bus),
                    static_cast<uint8_t>(slot), static_cast<uint8_t>(function),
                    programming_interface);
            }
        }
    }
    if (controller_count == 0u)
        starlix::drivers::serial::print("[ATA] PCI_IDE_CONTROLLER=NOT_FOUND\n");
    else if (g_device_count == 0u)
        starlix::drivers::serial::print("[ATA] IDENTIFY=NO_SUPPORTED_ATA_DEVICE\n");
    else {
        starlix::drivers::serial::print("[ATA] IDENTIFY=PASS; devices=");
        print_dec(g_device_count);
        starlix::drivers::serial::print("\n");
    }
    return g_device_count != 0u;
}

size_t ata_pio_device_count() { return g_device_count; }

bool ata_pio_device_get(size_t index, AtaPioDeviceInfo* output) {
    if (!output || index >= g_device_count) return false;
    *output = g_devices[index];
    return true;
}

bool ata_pio_read_blocks(const AtaPioDeviceInfo& device, uint64_t lba,
                         uint16_t block_count, void* output,
                         uint32_t output_capacity) {
    if (device.channel > 1u || device.drive > 1u || !device.present ||
        !output || !policy::read_request_valid(device.present, device.last_lba,
            device.logical_sector_bytes, lba, block_count, output_capacity)) return false;
    const AtaChannel channel{device.command_base, device.control_port, true};
    const uint16_t command = channel.command_base;
    starlix::cpu::outb(channel.control_port, kDeviceControlNien);
    starlix::cpu::outb(static_cast<uint16_t>(command + 6u),
        static_cast<uint8_t>(0xE0u | (device.drive << 4u)));
    delay_400ns(channel.control_port);
    if (!wait_status(channel, false, nullptr)) return false;

    if (device.lba48) {
        if (lba > 0x0000FFFFFFFFFFFFull) return false;
        /* ATA-6 requires high-order task-file bytes before low-order bytes. */
        starlix::cpu::outb(static_cast<uint16_t>(command + 2u),
                           static_cast<uint8_t>(block_count >> 8u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 3u),
                           static_cast<uint8_t>(lba >> 24u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 4u),
                           static_cast<uint8_t>(lba >> 32u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 5u),
                           static_cast<uint8_t>(lba >> 40u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 2u),
                           static_cast<uint8_t>(block_count));
        starlix::cpu::outb(static_cast<uint16_t>(command + 3u),
                           static_cast<uint8_t>(lba));
        starlix::cpu::outb(static_cast<uint16_t>(command + 4u),
                           static_cast<uint8_t>(lba >> 8u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 5u),
                           static_cast<uint8_t>(lba >> 16u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 7u), kCommandReadSectorsExt);
    } else {
        if (lba > 0x0FFFFFFFull ||
            static_cast<uint64_t>(block_count) > 0x10000000ull - lba ||
            block_count > 255u) return false;
        starlix::cpu::outb(static_cast<uint16_t>(command + 6u),
            static_cast<uint8_t>(0xE0u | (device.drive << 4u) | ((lba >> 24u) & 0x0Fu)));
        delay_400ns(channel.control_port);
        starlix::cpu::outb(static_cast<uint16_t>(command + 2u),
                           static_cast<uint8_t>(block_count));
        starlix::cpu::outb(static_cast<uint16_t>(command + 3u), static_cast<uint8_t>(lba));
        starlix::cpu::outb(static_cast<uint16_t>(command + 4u), static_cast<uint8_t>(lba >> 8u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 5u), static_cast<uint8_t>(lba >> 16u));
        starlix::cpu::outb(static_cast<uint16_t>(command + 7u), kCommandReadSectors);
    }

    auto* destination = static_cast<uint8_t*>(output);
    const uint32_t words_per_sector = device.logical_sector_bytes / 2u;
    for (uint16_t sector = 0u; sector < block_count; ++sector) {
        if (!wait_status(channel, true, nullptr)) return false;
        for (uint32_t word = 0u; word < words_per_sector; ++word) {
            const uint16_t value = starlix::cpu::inw(command);
            destination[0] = static_cast<uint8_t>(value);
            destination[1] = static_cast<uint8_t>(value >> 8u);
            destination += 2u;
        }
    }
    return wait_status(channel, false, nullptr);
}

} // namespace starlix::drivers::ata
