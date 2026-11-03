/* Phase 18: fixed-capacity USB device model and class-driver binding policy. */
#pragma once
#include "ehci_usb_core_policy.hpp"
namespace starlix::drivers::ehci::device_model_policy {
enum class State : uint8_t { Empty = 0, Enumerating = 1, Configured = 2,
                             Removing = 3, Quarantined = 4 };
enum class Binding : uint8_t { Unbound = 0, HubManagement = 1,
                               MassStorageBOT = 2, VendorGenericAPI = 3,
                               HIDInput = 4 };
struct Identity {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t device_class;
    bool is_hub;
    bool valid_hub_descriptor;
    bool has_msc_bot_interface;
    bool has_hid_boot_input;
    bool approved_vendor_api;
};
constexpr Binding choose_binding(const Identity& id) {
    if (id.is_hub && id.valid_hub_descriptor)
        return Binding::HubManagement;
    if (id.has_msc_bot_interface) return Binding::MassStorageBOT;
    if (id.has_hid_boot_input) return Binding::HIDInput;
    if (id.device_class == 0xFFu && id.approved_vendor_api) return Binding::VendorGenericAPI;
    return Binding::Unbound;
}
constexpr bool publish_allowed(State previous, State next) {
    if (previous == State::Empty) return next == State::Enumerating;
    if (previous == State::Enumerating) return next == State::Configured || next == State::Removing;
    if (previous == State::Configured) return next == State::Removing;
    if (previous == State::Removing) return next == State::Empty || next == State::Quarantined;
    return false;
}
constexpr bool query_allowed(bool record_present, State state,
                             uint8_t address, uint32_t generation,
                             uint32_t current_generation) {
    return record_present && state == State::Configured && address > 0u && address < 128u &&
           generation != 0u && generation == current_generation;
}
constexpr bool self_test() {
    Identity hub{0x1234u,0x5678u,usb_core::CLASS_HUB,true,true,false,false,false};
    Identity bad_hub{0x1234u,0x5678u,usb_core::CLASS_HUB,true,false,false,false,false};
    Identity msc{0,0,0,false,false,true,false,false};
    Identity hid{0x046Du,0xC31Cu,0,false,false,false,true,false};
    Identity generic{0xCAFEu,0x1000u,0,false,false,false,false,false};
    Identity allowed_vendor{0x1234u,0x5678u,0xFFu,false,false,false,false,true};
    Identity blocked_vendor{0x1234u,0x5678u,0xFFu,false,false,false,false,false};
    if (choose_binding(hub) != Binding::HubManagement ||
        choose_binding(bad_hub) != Binding::Unbound ||
        choose_binding(msc) != Binding::MassStorageBOT ||
        choose_binding(hid) != Binding::HIDInput ||
        choose_binding(generic) != Binding::Unbound ||
        choose_binding(allowed_vendor) != Binding::VendorGenericAPI ||
        choose_binding(blocked_vendor) != Binding::Unbound) return false;
    if (!publish_allowed(State::Empty,State::Enumerating) ||
        !publish_allowed(State::Enumerating,State::Configured) ||
        !publish_allowed(State::Configured,State::Removing) ||
        !publish_allowed(State::Removing,State::Empty) ||
        !publish_allowed(State::Removing,State::Quarantined) ||
        publish_allowed(State::Quarantined,State::Empty)) return false;
    if (!query_allowed(true,State::Configured,3u,9u,9u) ||
        query_allowed(true,State::Removing,3u,9u,9u) ||
        query_allowed(true,State::Configured,3u,8u,9u) ||
        query_allowed(false,State::Configured,3u,9u,9u)) return false;
    return true;
}
} // namespace starlix::drivers::ehci::device_model_policy
