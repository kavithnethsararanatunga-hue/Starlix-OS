/* Root-port debounce, actions and stable transition policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::root_port_policy {
enum class Action : uint8_t { Ignore = 0, Enumerate = 1, Disconnect = 2 };
enum class State : uint8_t { Disconnected = 0, Debouncing = 1, Resetting = 2,
                             Enabled = 3, Enumerating = 4, Configured = 5,
                             CompanionOwned = 6, Removing = 7, Failed = 8 };
constexpr Action classify(bool connected, bool enumerated, bool companion_owned) {
    if (!connected) return enumerated ? Action::Disconnect : Action::Ignore;
    if (enumerated || companion_owned) return Action::Ignore;
    return Action::Enumerate;
}
constexpr bool debounce_confirmed(bool first_connected, bool second_connected,
                                  bool expected_connected) {
    return first_connected == expected_connected &&
           second_connected == expected_connected &&
           first_connected == second_connected;
}
constexpr bool reset_allowed(bool connected, bool already_enabled, bool companion_owned,
                             bool debounce_passed) {
    return connected && !already_enabled && !companion_owned && debounce_passed;
}
constexpr bool self_test() {
    if (classify(false,false,false) != Action::Ignore ||
        classify(true,false,false) != Action::Enumerate ||
        classify(false,true,false) != Action::Disconnect ||
        classify(true,true,false) != Action::Ignore ||
        classify(true,false,true) != Action::Ignore) return false;
    if (!debounce_confirmed(true,true,true) || !debounce_confirmed(false,false,false) ||
        debounce_confirmed(true,false,true) || debounce_confirmed(false,true,false)) return false;
    if (!reset_allowed(true,false,false,true) || reset_allowed(false,false,false,true) ||
        reset_allowed(true,true,false,true) || reset_allowed(true,false,true,true) ||
        reset_allowed(true,false,false,false)) return false;
    return true;
}
}
