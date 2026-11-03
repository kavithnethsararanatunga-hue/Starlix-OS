/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#include "pci.hpp"
#include "serial.hpp"

namespace starlix::drivers::pci {

constexpr uint16_t PCI_CONFIG_ADDRESS = 0xCF8;
constexpr uint16_t PCI_CONFIG_DATA    = 0xCFC;
constexpr uint8_t PCI_CLASS_SERIAL_BUS = 0x0C;
constexpr uint8_t PCI_SUBCLASS_USB     = 0x03;
constexpr uint8_t PCI_PROGIF_EHCI      = 0x20;

static void print_hex32(uint32_t val) {
    const char* hex = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 7; i >= 0; i--) {
        buf[2 + i] = hex[val & 0xF];
        val >>= 4;
    }
    starlix::drivers::serial::print(buf);
}

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    const uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(slot) << 11) |
                             (static_cast<uint32_t>(func) << 8) |
                             (static_cast<uint32_t>(offset) & 0xFCu) | 0x80000000u;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS, address);
    return starlix::cpu::inl(PCI_CONFIG_DATA);
}

// Capture a generic PCI function descriptor without changing configuration
// space. BAR sizes are deliberately not probed here: sizing requires writes.
bool pci_query_device(uint8_t bus, uint8_t slot, uint8_t func,
                      StarlixPciDeviceInfoV1& output) {
    output = StarlixPciDeviceInfoV1{};
    if (slot >= 32u || func >= 8u) return false;

    const uint32_t vendor_device = pci_read(bus, slot, func, 0x00u);
    const uint16_t vendor = static_cast<uint16_t>(vendor_device & 0xFFFFu);
    if (vendor == 0xFFFFu || vendor == 0x0000u) return false;

    const uint32_t class_revision = pci_read(bus, slot, func, 0x08u);
    const uint32_t command_status = pci_read(bus, slot, func, 0x04u);
    const uint32_t header_register = pci_read(bus, slot, func, 0x0Cu);
    const uint8_t raw_header_type =
        static_cast<uint8_t>((header_register >> 16u) & 0xFFu);
    const uint8_t header_type = static_cast<uint8_t>(raw_header_type & 0x7Fu);

    output.struct_size = static_cast<StPciU32>(sizeof(output));
    output.abi_major = STARLIX_PCI_DEVICE_ABI_MAJOR;
    output.abi_minor = STARLIX_PCI_DEVICE_ABI_MINOR;
    output.bus = bus;
    output.device = slot;
    output.function = func;
    output.header_type = header_type;
    output.vendor_id = vendor;
    output.device_id = static_cast<uint16_t>(vendor_device >> 16u);
    output.command = static_cast<uint16_t>(command_status & 0xFFFFu);
    output.status = static_cast<uint16_t>(command_status >> 16u);
    output.class_code = static_cast<uint8_t>(class_revision >> 24u);
    output.subclass = static_cast<uint8_t>(class_revision >> 16u);
    output.programming_interface = static_cast<uint8_t>(class_revision >> 8u);
    output.revision_id = static_cast<uint8_t>(class_revision & 0xFFu);
    output.flags = StarlixPciDevicePresentV1;

    if ((raw_header_type & 0x80u) != 0u)
        output.flags |= StarlixPciDeviceMultifunctionV1;
    if ((output.command & (1u << 1u)) != 0u)
        output.flags |= StarlixPciDeviceMemoryDecodeEnabledV1;
    if ((output.command & (1u << 2u)) != 0u)
        output.flags |= StarlixPciDeviceBusMasterEnabledV1;

    uint32_t bar_count = 0u;
    if (header_type == 0x00u) bar_count = 6u;
    else if (header_type == 0x01u) bar_count = 2u;
    else if (header_type == 0x02u) bar_count = 1u;

    for (uint32_t index = 0u; index < bar_count; ++index) {
        const uint8_t offset = static_cast<uint8_t>(0x10u + index * 4u);
        const uint32_t raw_low = pci_read(bus, slot, func, offset);
        uint32_t raw_high = 0u;
        const bool is_memory = (raw_low & 1u) == 0u;
        const uint32_t memory_type = (raw_low >> 1u) & 0x3u;
        const bool is_64_bit = raw_low != 0u && raw_low != 0xFFFFFFFFu &&
                               is_memory && memory_type == 0x2u;
        if (is_64_bit && index + 1u < bar_count)
            raw_high = pci_read(bus, slot, func,
                static_cast<uint8_t>(offset + 4u));

        (void)StarlixPciDecodeBarV1(
            raw_low, raw_high, index, bar_count, &output.bars[index]);

        if (is_64_bit && index + 1u < bar_count) {
            StarlixPciBarInfoV1& continuation = output.bars[index + 1u];
            continuation = StarlixPciBarInfoV1{};
            continuation.raw_low = raw_high;
            continuation.flags = StarlixPciBarContinuationV1;
            ++index;
        }
    }

    return true;
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    const uint32_t address = (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(slot) << 11) |
                             (static_cast<uint32_t>(func) << 8) |
                             (static_cast<uint32_t>(offset) & 0xFCu) | 0x80000000u;
    starlix::cpu::outl(PCI_CONFIG_ADDRESS, address);
    starlix::cpu::outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_scan_all(PciDeviceVisitorV1 visitor, void* context) {
    starlix::drivers::serial::print("\n--- STARLIX PCI BUS ENUMERATION ---\n");
    uint32_t count = 0u;
    bool stop = false;
    for (uint32_t bus = 0u; bus < 256u && !stop; ++bus) {
        for (uint32_t slot = 0u; slot < 32u && !stop; ++slot) {
            for (uint32_t func = 0u; func < 8u; ++func) {
                StarlixPciDeviceInfoV1 device{};
                if (!pci_query_device(static_cast<uint8_t>(bus),
                        static_cast<uint8_t>(slot), static_cast<uint8_t>(func),
                        device)) continue;

                starlix::drivers::serial::print("[PCI] ");
                print_hex32(device.bus);
                starlix::drivers::serial::print(":");
                print_hex32(device.device);
                starlix::drivers::serial::print(".");
                print_hex32(device.function);
                starlix::drivers::serial::print(" Vendor:");
                print_hex32(device.vendor_id);
                starlix::drivers::serial::print(" Device:");
                print_hex32(device.device_id);
                starlix::drivers::serial::print(" Class:");
                print_hex32(device.class_code);
                starlix::drivers::serial::print(" Subclass:");
                print_hex32(device.subclass);
                starlix::drivers::serial::print(" ProgIF:");
                print_hex32(device.programming_interface);
                starlix::drivers::serial::print(" BAR0:");
                print_hex32(device.bars[0].raw_low);
                starlix::drivers::serial::print("\n");
                ++count;

                // A visitor may stop after binding a driver. The scanner itself
                // remains vendor-neutral and supplies only a read-only snapshot.
                if (visitor && !visitor(device, context)) {
                    stop = true;
                    break;
                }
            }
        }
    }
    starlix::drivers::serial::print(stop
        ? "[PCI] Enumeration stopped by device visitor.\n"
        : "[PCI] Enumeration complete.\n");
    return count;
}

bool pci_find_ehci(uint8_t& bus_out, uint8_t& slot_out, uint8_t& func_out, uint32_t& bar0_out) {
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t slot = 0; slot < 32; ++slot) {
            for (uint32_t func = 0; func < 8; ++func) {
                const uint32_t vendor_device = pci_read(bus, slot, func, 0x00);
                const uint16_t vendor = static_cast<uint16_t>(vendor_device & 0xFFFFu);
                if (vendor == 0xFFFFu || vendor == 0x0000u) continue;

                const uint32_t class_rev = pci_read(bus, slot, func, 0x08);
                const uint8_t class_code = static_cast<uint8_t>(class_rev >> 24);
                const uint8_t subclass = static_cast<uint8_t>(class_rev >> 16);
                const uint8_t prog_if = static_cast<uint8_t>(class_rev >> 8);

                /* EHCI is class 0x0C, subclass 0x03, programming interface 0x20.
                   UHCI/OHCI/xHCI must not be mistaken for an EHCI controller. */
                if (class_code != PCI_CLASS_SERIAL_BUS ||
                    subclass != PCI_SUBCLASS_USB ||
                    prog_if != PCI_PROGIF_EHCI) {
                    continue;
                }

                const uint32_t bar0_raw = pci_read(bus, slot, func, 0x10);
                if (bar0_raw == 0 || bar0_raw == 0xFFFFFFFFu || (bar0_raw & 1u)) continue;

                const uint32_t bar_type = (bar0_raw >> 1) & 0x3u;
                if (bar_type == 0x1u || bar_type == 0x3u) continue; /* unsupported below-1MiB / reserved types */
                if (bar_type == 0x2u) {
                    /* This kernel has no >4-GiB DMA/MMIO mapping implementation yet. */
                    const uint32_t bar0_high = pci_read(bus, slot, func, 0x14);
                    if (bar0_high != 0) continue;
                }

                const uint32_t base = bar0_raw & 0xFFFFFFF0u;
                if (base == 0 || base == 0xFFFFFFF0u) continue;

                bus_out = static_cast<uint8_t>(bus);
                slot_out = static_cast<uint8_t>(slot);
                func_out = static_cast<uint8_t>(func);
                bar0_out = base;
                return true;
            }
        }
    }
    return false;
}

bool pci_find_uhci(uint8_t& bus_out, uint8_t& slot_out, uint8_t& func_out, uint32_t& bar4_out) {
    for (uint32_t bus = 0; bus < 256; ++bus) {
        for (uint32_t slot = 0; slot < 32; ++slot) {
            for (uint32_t func = 0; func < 8; ++func) {
                const uint32_t vd = pci_read(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                             static_cast<uint8_t>(func), 0x00u);
                if (static_cast<uint16_t>(vd) == 0xFFFFu || static_cast<uint16_t>(vd) == 0u) continue;
                const uint32_t class_rev = pci_read(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                                     static_cast<uint8_t>(func), 0x08u);
                if (static_cast<uint8_t>(class_rev >> 24u) != PCI_CLASS_SERIAL_BUS ||
                    static_cast<uint8_t>(class_rev >> 16u) != PCI_SUBCLASS_USB ||
                    static_cast<uint8_t>(class_rev >> 8u) != 0x00u) continue;
                const uint32_t raw = pci_read(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                              static_cast<uint8_t>(func), 0x20u);
                if ((raw & 1u) == 0u || raw == 0xFFFFFFFFu) continue;
                const uint32_t decoded = raw & 0xFFFFFFFCu;
                if (decoded == 0u || decoded > 0xFFE0u) continue;
                bus_out = static_cast<uint8_t>(bus);
                slot_out = static_cast<uint8_t>(slot);
                func_out = static_cast<uint8_t>(func);
                bar4_out = decoded;
                return true;
            }
        }
    }
    return false;
}

} // namespace starlix::drivers::pci
