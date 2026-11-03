#include "../kernel/src/loader/user_host_service_policy.hpp"

using namespace starlix::loader::host_service::policy;

int main() {
    if (!service_abi_version_valid(SERVICE_ABI_VERSION)) return 12;
    if (service_abi_version_valid(0ULL) || service_abi_version_valid(1ULL) ||
        service_abi_version_valid(2ULL)) return 13;
    if (!service_allowed(static_cast<unsigned long long>(Service::PollInput))) return 1;
    if (!service_allowed(static_cast<unsigned long long>(Service::Authenticate))) return 2;
    if (!service_allowed(static_cast<unsigned long long>(Service::RequestDesktop))) return 3;
    if (!service_allowed(static_cast<unsigned long long>(Service::Yield))) return 4;
    if (!service_allowed(static_cast<unsigned long long>(Service::DisplayInfo))) return 5;
    if (!service_allowed(static_cast<unsigned long long>(Service::DisplayBlit))) return 6;
    if (!service_allowed(static_cast<unsigned long long>(Service::FrameComplete))) return 7;
    if (!service_allowed(static_cast<unsigned long long>(Service::MonotonicMilliseconds))) return 14;
    if (service_allowed(10ULL + 1ULL)) return 15;
    if (service_allowed(0ULL) || service_allowed(2ULL) || service_allowed(3ULL) ||
        service_allowed(0x80ULL) || service_allowed(0xFFFFFFFFULL)) return 8;
    if (authentication_service_available() || desktop_handoff_available()) return 9;
    if (auth_service_unavailable_result() != 2ULL) return 10;
    if (desktop_handoff_denied_result() != 0ULL) return 11;
    return 0;
}
