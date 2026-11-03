/* Phase 17: safe root-port suspend/resume admission policy. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::power_policy {
enum class Result : uint8_t { Allowed = 0, Invalid = 1, Disconnected = 2,
    Disabled = 3, CompanionOwned = 4, AlreadyInState = 5, Busy = 6,
    Timeout = 7 };
constexpr Result suspend_admission(bool connected, bool enabled, bool companion_owned,
                                   bool already_suspended, bool transfer_engine_idle,
                                   bool async_idle, bool periodic_idle, bool requests_drained) {
    if (!connected) return Result::Disconnected;
    if (!enabled) return Result::Disabled;
    if (companion_owned) return Result::CompanionOwned;
    if (already_suspended) return Result::AlreadyInState;
    if (!transfer_engine_idle || !async_idle || !periodic_idle || !requests_drained)
        return Result::Busy;
    return Result::Allowed;
}
constexpr Result resume_admission(bool connected, bool enabled, bool companion_owned,
                                 bool is_suspended) {
    if (!connected) return Result::Disconnected;
    if (companion_owned) return Result::CompanionOwned;
    if (!enabled) return Result::Disabled;
    if (!is_suspended) return Result::AlreadyInState;
    return Result::Allowed;
}
constexpr bool suspend_confirmed(bool connected, bool enabled, bool suspended) {
    return connected && enabled && suspended;
}
constexpr bool resume_confirmed(bool connected, bool suspended, bool resume_signal_set) {
    return connected && !suspended && !resume_signal_set;
}
constexpr bool self_test() {
    if (suspend_admission(true,true,false,false,true,true,true,true) != Result::Allowed ||
        suspend_admission(false,true,false,false,true,true,true,true) != Result::Disconnected ||
        suspend_admission(true,false,false,false,true,true,true,true) != Result::Disabled ||
        suspend_admission(true,true,true,false,true,true,true,true) != Result::CompanionOwned ||
        suspend_admission(true,true,false,true,true,true,true,true) != Result::AlreadyInState ||
        suspend_admission(true,true,false,false,false,true,true,true) != Result::Busy ||
        suspend_admission(true,true,false,false,true,false,true,true) != Result::Busy ||
        suspend_admission(true,true,false,false,true,true,false,true) != Result::Busy ||
        suspend_admission(true,true,false,false,true,true,true,false) != Result::Busy) return false;
    if (resume_admission(true,true,false,true) != Result::Allowed ||
        resume_admission(false,true,false,true) != Result::Disconnected ||
        resume_admission(true,true,true,true) != Result::CompanionOwned ||
        resume_admission(true,false,false,true) != Result::Disabled ||
        resume_admission(true,true,false,false) != Result::AlreadyInState) return false;
    if (!suspend_confirmed(true,true,true) || suspend_confirmed(false,true,true) ||
        !resume_confirmed(true,false,false) || resume_confirmed(true,true,false) ||
        resume_confirmed(true,false,true)) return false;
    return true;
}
} // namespace starlix::drivers::ehci::power_policy
