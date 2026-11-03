/* Bounded device-address lifetime policy. A removed address is reusable only
   after all software requests are terminal/released and periodic QHs are closed. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::device_lifetime {
enum class State : uint8_t { Free = 0, Enumerating = 1, Configured = 2,
                             Removing = 3, Quarantined = 4 };
constexpr bool transition_allowed(State from, State to) {
    if (from == State::Free) return to == State::Enumerating;
    if (from == State::Enumerating) return to == State::Configured || to == State::Removing;
    if (from == State::Configured) return to == State::Removing;
    if (from == State::Removing) return to == State::Free || to == State::Quarantined;
    return false; /* Quarantined is deliberately terminal until reboot. */
}
constexpr bool address_reusable(State state, bool requests_drained, bool endpoints_closed) {
    return state == State::Removing && requests_drained && endpoints_closed;
}
constexpr uint32_t next_generation(uint32_t generation) {
    const uint32_t next = generation + 1u;
    return next == 0u ? 1u : next;
}
constexpr bool reference_valid(uint8_t address, uint32_t reference_generation,
                               uint32_t current_generation, State state) {
    return address != 0u && address <= 127u && reference_generation != 0u &&
           reference_generation == current_generation && state == State::Configured;
}
constexpr bool self_test() {
    if (!transition_allowed(State::Free, State::Enumerating) ||
        !transition_allowed(State::Enumerating, State::Configured) ||
        !transition_allowed(State::Configured, State::Removing) ||
        !transition_allowed(State::Removing, State::Free) ||
        !transition_allowed(State::Removing, State::Quarantined)) return false;
    if (transition_allowed(State::Free, State::Configured) ||
        transition_allowed(State::Quarantined, State::Free) ||
        address_reusable(State::Removing, false, true) ||
        address_reusable(State::Removing, true, false) ||
        !address_reusable(State::Removing, true, true)) return false;
    if (next_generation(1u) != 2u || next_generation(0xFFFFFFFFu) != 1u) return false;
    if (!reference_valid(2u,7u,7u,State::Configured) ||
        reference_valid(2u,6u,7u,State::Configured) ||
        reference_valid(2u,7u,7u,State::Removing) ||
        reference_valid(0u,7u,7u,State::Configured)) return false;
    return true;
}
}
