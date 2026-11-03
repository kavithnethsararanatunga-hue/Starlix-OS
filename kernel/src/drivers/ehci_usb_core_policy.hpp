/* Starlix USB core: bounded descriptor validation and enumeration policy. */
#pragma once
#include "ehci_spec.hpp"
#include "ehci_security_policy.hpp"
#include "ehci_class_registry_policy.hpp"

namespace starlix::drivers::ehci::usb_core {

static constexpr uint8_t DESC_STRING = 3u;
static constexpr uint8_t DESC_HUB = 0x29u;
static constexpr uint8_t CLASS_HUB = 9u;
static constexpr uint16_t MAX_CONFIGURATION_BYTES = 4096u;
static constexpr uint8_t MAX_SUPPORTED_HUB_PORTS = 15u;

struct ConfigurationSummary {
    uint8_t configuration_value;
    uint8_t declared_interfaces;
    uint8_t distinct_interfaces;
    uint16_t endpoint_count;
    bool has_hub_interface;
    uint8_t hub_interface_number;
    uint8_t hub_interface_protocol;
    bool has_msc_bot_interface;
    bool has_hid_boot_input;
    uint8_t msc_interface_number;
    uint8_t classified_interface_count;
    uint8_t unsupported_interface_count;
    uint32_t class_binding_mask;
};

struct HubDescriptorSummary {
    uint8_t port_count;
    uint16_t characteristics;
    uint8_t power_good_delay_2ms;
    uint8_t controller_current_ma;
    uint8_t removable_bitmap_bytes;
};

inline bool validate_device_descriptor_prefix(const uint8_t* b, uint16_t available,
                                              bool high_speed) {
    return b && available >= 8u && b[0] == 18u && b[1] == USB_DESC_DEVICE &&
           (!high_speed || b[7] == 64u);
}

inline bool validate_device_descriptor(const uint8_t* b, uint16_t available,
                                       bool high_speed) {
    if (!b || available < 18u || b[0] != 18u || b[1] != USB_DESC_DEVICE ||
        b[17] == 0u || (high_speed && b[7] != 64u)) return false;
    if (b[7] != 8u && b[7] != 16u && b[7] != 32u && b[7] != 64u) return false;
    return true;
}

inline bool validate_string_descriptor(const uint8_t* b, uint16_t available) {
    if (!b || available < 2u || b[1] != DESC_STRING) return false;
    return security_policy::string_descriptor_valid(b, available);
}

inline bool parse_configuration(const uint8_t* b, uint16_t total_length,
                                ConfigurationSummary* out) {
    if (!b || !out || total_length < 9u || total_length > MAX_CONFIGURATION_BYTES)
        return false;
    const uint8_t config_length = b[0];
    if (config_length < 9u || config_length > total_length ||
        b[1] != USB_DESC_CONFIGURATION) return false;
    const uint16_t declared = static_cast<uint16_t>(b[2] |
        (static_cast<uint16_t>(b[3]) << 8));
    if (declared != total_length || b[4] == 0u || b[5] == 0u ||
        (b[7] & 0x80u) == 0u) return false;

    uint8_t seen_interfaces[32]{};
    uint8_t distinct_interfaces = 0u;
    uint16_t endpoint_total = 0u;
    bool inside_interface = false;
    uint8_t current_interface = 0u;
    uint8_t current_alt = 0u;
    uint8_t expected_endpoints = 0u;
    uint8_t observed_endpoints = 0u;
    uint8_t endpoint_seen[4]{};
    ConfigurationSummary summary{};
    summary.configuration_value = b[5];
    summary.declared_interfaces = b[4];

    uint16_t offset = config_length;
    while (offset < total_length) {
        if (static_cast<uint16_t>(total_length - offset) < 2u) return false;
        const uint8_t len = b[offset];
        const uint8_t type = b[offset + 1u];
        uint32_t checked_next = 0u;
        if (!security_policy::advance_descriptor(total_length, offset, len, &checked_next)) return false;

        if (type == USB_DESC_INTERFACE) {
            if (len < 9u) return false;
            if (inside_interface && observed_endpoints != expected_endpoints) return false;
            inside_interface = true;
            current_interface = b[offset + 2u];
            current_alt = b[offset + 3u];
            (void)current_alt;
            expected_endpoints = b[offset + 4u];
            observed_endpoints = 0u;
            endpoint_seen[0] = endpoint_seen[1] = endpoint_seen[2] = endpoint_seen[3] = 0u;
            const uint8_t mask = static_cast<uint8_t>(1u << (current_interface & 7u));
            uint8_t& byte = seen_interfaces[current_interface >> 3u];
            if ((byte & mask) == 0u) {
                byte = static_cast<uint8_t>(byte | mask);
                if (distinct_interfaces == 0xFFu) return false;
                ++distinct_interfaces;
            }
            const class_registry::Decision decision = class_registry::classify(
                {b[offset + 5u], b[offset + 6u], b[offset + 7u]});
            if (decision.driver == class_registry::Driver::Unsupported ||
                decision.driver == class_registry::Driver::FirmwareUpdateRestricted) {
                if (summary.unsupported_interface_count != 0xFFu) ++summary.unsupported_interface_count;
            } else {
                if (summary.classified_interface_count != 0xFFu) ++summary.classified_interface_count;
                const uint8_t driver_id = static_cast<uint8_t>(decision.driver);
                if (driver_id < 32u) summary.class_binding_mask |= (1u << driver_id);
            }
            if (b[offset + 5u] == CLASS_HUB && !summary.has_hub_interface) {
                summary.has_hub_interface = true;
                summary.hub_interface_number = current_interface;
                summary.hub_interface_protocol = b[offset + 7u];
            }
            if (b[offset + 5u] == 0x03u && b[offset + 6u] == 1u &&
                (b[offset + 7u] == 1u || b[offset + 7u] == 2u) &&
                b[offset + 3u] == 0u) {
                summary.has_hid_boot_input = true;
            }
            if (b[offset + 5u] == USB_CLASS_MASS_STORAGE &&
                b[offset + 6u] == USB_MSC_SUBCLASS_SCSI &&
                b[offset + 7u] == USB_MSC_PROTOCOL_BOT && b[offset + 3u] == 0u &&
                !summary.has_msc_bot_interface) {
                summary.has_msc_bot_interface = true;
                summary.msc_interface_number = current_interface;
            }
        } else if (type == USB_DESC_ENDPOINT) {
            if (!inside_interface || len < 7u) return false;
            const uint8_t address = b[offset + 2u];
            const uint8_t number = static_cast<uint8_t>(address & 0x0Fu);
            const uint8_t attributes = b[offset + 3u];
            const uint16_t raw_packet = static_cast<uint16_t>(b[offset + 4u] |
                (static_cast<uint16_t>(b[offset + 5u]) << 8));
            const uint16_t packet = static_cast<uint16_t>(raw_packet & 0x07FFu);
            const uint8_t transfer_type = static_cast<uint8_t>(attributes & 0x03u);
            if (!security_policy::endpoint_valid(address, attributes, packet, 2u) ||
                (address & 0x70u) != 0u || number == 0u || packet == 0u ||
                packet > 1024u || (raw_packet & 0xE000u) != 0u) return false;
            if (transfer_type == 2u && packet > 512u) return false;
            const uint8_t additional_transactions = static_cast<uint8_t>((raw_packet >> 11u) & 0x03u);
            if ((transfer_type == 0u || transfer_type == 2u) && additional_transactions != 0u)
                return false;
            if ((transfer_type == 1u || transfer_type == 3u) && additional_transactions == 3u)
                return false;
            const uint8_t index = static_cast<uint8_t>(number * 2u + ((address & 0x80u) ? 1u : 0u));
            const uint8_t byte_index = static_cast<uint8_t>(index >> 3u);
            const uint8_t bit = static_cast<uint8_t>(1u << (index & 7u));
            if ((endpoint_seen[byte_index] & bit) != 0u) return false;
            endpoint_seen[byte_index] = static_cast<uint8_t>(endpoint_seen[byte_index] | bit);
            if (observed_endpoints == 0xFFu || endpoint_total == 0xFFFFu) return false;
            ++observed_endpoints;
            ++endpoint_total;
        } else if (type == USB_DESC_CONFIGURATION) {
            /* A second configuration descriptor inside this stream is malformed. */
            return false;
        }
        offset = static_cast<uint16_t>(checked_next);
    }
    if (inside_interface && observed_endpoints != expected_endpoints) return false;
    if (distinct_interfaces != summary.declared_interfaces) return false;
    summary.distinct_interfaces = distinct_interfaces;
    summary.endpoint_count = endpoint_total;
    *out = summary;
    return true;
}

inline bool validate_hub_descriptor(const uint8_t* b, uint16_t available,
                                    HubDescriptorSummary* out) {
    if (!b || !out || available < 7u || b[1] != DESC_HUB) return false;
    const uint8_t length = b[0];
    const uint8_t ports = b[2];
    if (ports == 0u || ports > MAX_SUPPORTED_HUB_PORTS || length > available) return false;
    const uint8_t bitmap_bytes = static_cast<uint8_t>((ports + 1u + 7u) / 8u);
    const uint8_t minimum = static_cast<uint8_t>(7u + 2u * bitmap_bytes);
    if (length < minimum) return false;
    out->port_count = ports;
    out->characteristics = static_cast<uint16_t>(b[3] |
        (static_cast<uint16_t>(b[4]) << 8));
    out->power_good_delay_2ms = b[5];
    out->controller_current_ma = b[6];
    out->removable_bitmap_bytes = bitmap_bytes;
    return true;
}

inline bool configuration_self_test() {
    uint8_t cfg[] = {9,2,25,0,1,1,0,0x80,50, 9,4,0,0,1,8,6,0x50,0,
                     7,5,0x81,2,0,2,0};
    ConfigurationSummary summary{};
    if (!parse_configuration(cfg, sizeof(cfg), &summary) ||
        summary.distinct_interfaces != 1u || summary.endpoint_count != 1u ||
        summary.classified_interface_count != 1u || summary.unsupported_interface_count != 0u ||
        !summary.has_msc_bot_interface || summary.msc_interface_number != 0u) return false;
    cfg[14] = 0x99u; /* unknown class remains unbound, while other interfaces can still enumerate */
    if (!parse_configuration(cfg, sizeof(cfg), &summary) || summary.unsupported_interface_count != 1u ||
        summary.classified_interface_count != 0u || summary.has_msc_bot_interface) return false;
    cfg[14] = 8u;
    cfg[20] = 0u; /* endpoint zero is invalid in a configuration interface */
    if (parse_configuration(cfg, sizeof(cfg), &summary)) return false;
    cfg[20] = 0x81u;
    cfg[2] = 24u; /* declared/available total-length mismatch */
    if (parse_configuration(cfg, sizeof(cfg), &summary)) return false;
    const uint8_t hid_cfg[] = {9,2,25,0,1,1,0,0x80,25,
        9,4,0,0,1,3,1,1,0, 7,5,0x81,3,8,0,10};
    if (!parse_configuration(hid_cfg, sizeof(hid_cfg), &summary) ||
        !summary.has_hid_boot_input || summary.has_msc_bot_interface ||
        summary.endpoint_count != 1u) return false;
    return true;
}

} // namespace starlix::drivers::ehci::usb_core
