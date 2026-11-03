/* USB 2.0 hub descriptor and speed-routing policy. Split transactions are not
   claimed until QH hub-address/port and TT scheduling are implemented. */
#pragma once
#include "ehci_usb_core_policy.hpp"
namespace starlix::drivers::ehci::hub_policy {
enum class Speed : uint8_t { Low = 1, Full = 2, High = 3 };
enum class Route : uint8_t { DirectEhci = 0, HighSpeedHubChild = 1,
                             CompanionHandoff = 2, SplitTransactionRequired = 3,
                             Unsupported = 4, InvalidTopology = 5,
                             CompanionHubChild = 6 };
enum class PortAction : uint8_t { NoDevice = 0, KeepChild = 1, Attach = 2, Detach = 3 };
struct PortState { bool connected; bool enabled; bool low_speed; bool high_speed; uint16_t changes; };
constexpr PortState decode_port_status(const uint8_t bytes[4]) {
    if (!bytes) return {};
    const uint16_t status = static_cast<uint16_t>(bytes[0] |
        (static_cast<uint16_t>(bytes[1]) << 8u));
    return { (status & 1u) != 0u, (status & 2u) != 0u,
             (status & (1u << 9u)) != 0u, (status & (1u << 10u)) != 0u,
             static_cast<uint16_t>(bytes[2] | (static_cast<uint16_t>(bytes[3]) << 8u)) };
}
constexpr PortAction port_action(const PortState& state, bool child_exists) {
    if (!state.connected) return child_exists ? PortAction::Detach : PortAction::NoDevice;
    return child_exists ? PortAction::KeepChild : PortAction::Attach;
}
static constexpr uint8_t MAX_TOPOLOGY_DEPTH = 4u;
constexpr Route route_device(bool parent_is_root, bool parent_is_hub,
                             Speed parent_speed, Speed child_speed,
                             bool hub_has_tt, bool companion_available,
                             uint8_t depth) {
    if (depth > MAX_TOPOLOGY_DEPTH ||
        static_cast<uint8_t>(child_speed) < 1u || static_cast<uint8_t>(child_speed) > 3u)
        return Route::InvalidTopology;
    if (parent_is_root) {
        if (depth != 1u) return Route::InvalidTopology;
        if (child_speed == Speed::High) return Route::DirectEhci;
        return companion_available ? Route::CompanionHandoff : Route::Unsupported;
    }
    if (!parent_is_hub || depth < 2u)
        return Route::InvalidTopology;
    if (parent_speed == Speed::Full) {
        if (child_speed == Speed::High) return Route::Unsupported;
        return companion_available ? Route::CompanionHubChild : Route::Unsupported;
    }
    if (parent_speed != Speed::High) return Route::InvalidTopology;
    if (child_speed == Speed::High) return Route::HighSpeedHubChild;
    return hub_has_tt ? Route::SplitTransactionRequired : Route::Unsupported;
}
constexpr bool topology_depth_valid(uint8_t depth) {
    return depth >= 1u && depth <= MAX_TOPOLOGY_DEPTH;
}
constexpr bool self_test() {
    const uint8_t connected_low_speed[4] = {3u, 2u, 1u, 0u};
    const PortState decoded = decode_port_status(connected_low_speed);
    if (!decoded.connected || !decoded.enabled || !decoded.low_speed || decoded.high_speed ||
        decoded.changes != 1u || port_action(decoded, false) != PortAction::Attach ||
        port_action(decoded, true) != PortAction::KeepChild) return false;
    const uint8_t disconnected[4] = {0u, 0u, 0u, 0u};
    if (port_action(decode_port_status(disconnected), true) != PortAction::Detach ||
        port_action(decode_port_status(disconnected), false) != PortAction::NoDevice) return false;
    const uint8_t connected_high_speed[4] = {1u, 4u, 0u, 0u};
    if (!decode_port_status(connected_high_speed).high_speed) return false;
    if (decode_port_status(nullptr).connected) return false;
    if (route_device(true,false,Speed::High,Speed::High,false,false,1u) != Route::DirectEhci)
        return false;
    if (route_device(true,false,Speed::High,Speed::Full,false,true,1u) != Route::CompanionHandoff)
        return false;
    if (route_device(true,false,Speed::High,Speed::Low,false,false,1u) != Route::Unsupported)
        return false;
    if (route_device(false,true,Speed::High,Speed::High,true,false,2u) != Route::HighSpeedHubChild)
        return false;
    if (route_device(false,true,Speed::High,Speed::Full,true,false,2u) != Route::SplitTransactionRequired)
        return false;
    if (route_device(false,true,Speed::High,Speed::Low,false,false,2u) != Route::Unsupported)
        return false;
    if (route_device(false,true,Speed::Full,Speed::Low,false,true,2u) != Route::CompanionHubChild ||
        route_device(false,true,Speed::Full,Speed::Full,false,true,2u) != Route::CompanionHubChild ||
        route_device(false,true,Speed::Full,Speed::High,false,true,2u) != Route::Unsupported ||
        route_device(false,true,Speed::Full,Speed::Low,false,false,2u) != Route::Unsupported)
        return false;
    if (route_device(false,true,Speed::Full,Speed::High,true,false,2u) != Route::Unsupported)
        return false;
    if (route_device(false,true,Speed::High,Speed::High,true,false,5u) != Route::InvalidTopology)
        return false;
    return topology_depth_valid(1u) && topology_depth_valid(4u) &&
           !topology_depth_valid(0u) && !topology_depth_valid(5u);
}
}
