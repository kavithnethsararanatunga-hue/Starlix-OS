// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::loader::host_service::policy {

constexpr unsigned long long SERVICE_ABI_VERSION = 3ULL;
constexpr bool service_abi_version_valid(unsigned long long value) {
    return value == SERVICE_ABI_VERSION;
}

enum class Service : unsigned long long {
    PollInput = 1ULL,
    Authenticate = 4ULL,
    RequestDesktop = 5ULL,
    Yield = 6ULL,
    DisplayInfo = 7ULL,
    DisplayBlit = 8ULL,
    FrameComplete = 9ULL,
    MonotonicMilliseconds = 10ULL,
};

constexpr bool service_allowed(unsigned long long value) {
    return value == static_cast<unsigned long long>(Service::PollInput) ||
           value == static_cast<unsigned long long>(Service::Authenticate) ||
           value == static_cast<unsigned long long>(Service::RequestDesktop) ||
           value == static_cast<unsigned long long>(Service::Yield) ||
           value == static_cast<unsigned long long>(Service::DisplayInfo) ||
           value == static_cast<unsigned long long>(Service::DisplayBlit) ||
           value == static_cast<unsigned long long>(Service::FrameComplete) ||
           value == static_cast<unsigned long long>(Service::MonotonicMilliseconds);
}

/* These remain false until trusted lsass/authentication and session-manager
   endpoints are implemented and independently validated. */
constexpr bool authentication_service_available() { return false; }
constexpr bool desktop_handoff_available() { return false; }
constexpr unsigned long long auth_service_unavailable_result() { return 2ULL; }
constexpr unsigned long long desktop_handoff_denied_result() { return 0ULL; }

} // namespace starlix::loader::host_service::policy
