/* Pure UHCI validation helpers shared by the kernel and host policy tests. */
#pragma once
#include "ehci_spec.hpp"

namespace starlix::drivers::uhci::policy {

constexpr bool io_bar_valid(uint32_t raw, uint16_t* decoded = nullptr) {
    if ((raw & 1u) == 0u || raw == 0xFFFFFFFFu) return false;
    const uint16_t base = static_cast<uint16_t>(raw & 0xFFFCu);
    if (base == 0u || base > 0xFFE0u) return false;
    if (decoded) *decoded = base;
    return true;
}

constexpr bool dma_page_valid(uint32_t address, uint32_t bytes, uint32_t alignment) {
    return bytes != 0u && alignment != 0u && (alignment & (alignment - 1u)) == 0u &&
        (address & (alignment - 1u)) == 0u &&
        static_cast<uint64_t>(address) + bytes - 1u <= 0xFFFFFFFFull &&
        ((address & 0xFFFu) + bytes) <= 0x1000u;
}

constexpr uint32_t link_td(uint32_t address) { return address & ~0xFu; }
constexpr uint32_t link_qh(uint32_t address) { return (address & ~0xFu) | 2u; }
constexpr bool link_terminated(uint32_t link) { return (link & 1u) != 0u; }
constexpr uint16_t actual_length(uint32_t status) {
    const uint16_t encoded = static_cast<uint16_t>(status & 0x7FFu);
    return encoded == 0x7FFu ? 0u : static_cast<uint16_t>(encoded + 1u);
}
constexpr bool td_active(uint32_t status) { return (status & (1u << 23u)) != 0u; }
constexpr bool td_failed(uint32_t status) {
    return (status & ((1u << 17u) | (1u << 18u) | (1u << 20u) | (1u << 21u) | (1u << 22u))) != 0u;
}
constexpr bool td_nak(uint32_t status) { return (status & (1u << 19u)) != 0u; }
constexpr bool port_connected(uint16_t status) { return (status & 1u) != 0u; }
constexpr bool port_low_speed(uint16_t status) { return (status & (1u << 8u)) != 0u; }
constexpr bool port_change(uint16_t status) { return (status & 0x0Au) != 0u; }
constexpr bool packet_valid(bool low, uint8_t endpoint, uint16_t packet) {
    if (packet == 0u) return false;
    if (endpoint == 0u) return packet == 8u || (!low && (packet == 16u || packet == 32u || packet == 64u));
    return low ? packet <= 8u : packet <= 64u;
}
constexpr uint8_t frame_interval(uint8_t interval) { return interval ? interval : 1u; }

constexpr bool self_test() {
    uint16_t base = 0;
    if (!io_bar_valid(0xC001u, &base) || base != 0xC000u ||
        io_bar_valid(0xC000u) || io_bar_valid(0xFFE5u)) return false;
    if (!dma_page_valid(0x1000u, 4096u, 4096u) ||
        dma_page_valid(0x1FF0u, 32u, 16u) ||
        dma_page_valid(0x1001u, 16u, 16u)) return false;
    if (link_td(0x12345u) != 0x12340u || link_qh(0x12345u) != 0x12342u ||
        !link_terminated(1u) || link_terminated(0u)) return false;
    if (actual_length(0x7FFu) != 0u || actual_length(7u) != 8u ||
        !td_active(1u << 23u) || !td_failed(1u << 22u) || !td_nak(1u << 19u)) return false;
    if (!port_connected(1u) || port_connected(0u) || !port_low_speed(1u << 8u) ||
        !port_change(2u) || !port_change(8u) || port_change(1u)) return false;
    return packet_valid(true, 0u, 8u) && !packet_valid(true, 0u, 64u) &&
        packet_valid(false, 0u, 64u) && packet_valid(true, 1u, 8u) &&
        !packet_valid(true, 1u, 9u) && packet_valid(false, 1u, 64u) &&
        frame_interval(0u) == 1u && frame_interval(8u) == 8u;
}

} // namespace starlix::drivers::uhci::policy
