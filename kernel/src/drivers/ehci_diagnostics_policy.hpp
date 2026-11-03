/* Phase 31: stable counter IDs and monotonic saturating diagnostics helpers. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::diagnostics {
enum Counter : uint32_t {
    IRQCallbacks=0, EHCIIRQCauses, USBIntCauses, USBErrCauses, PortChangeCauses,
    HostErrorCauses, IAACauses, ControlCalls, ControlSuccess, ControlFailure,
    BulkCalls, BulkSuccess, BulkFailure, AsyncSubmitted, AsyncCompleted,
    AsyncFailed, AsyncTimeouts, AsyncCancelled, PeriodicCompleted, PeriodicErrors,
    IsoAttempts, IsoSuccess, IsoErrors, BytesIn, BytesOut, PortRescans,
    MaxControlLatencyMs, MaxBulkLatencyMs, MaxAsyncLatencyMs, ControllerStarts, Count
};
struct Snapshot { uint64_t value[Count]; };
constexpr uint64_t saturating_add(uint64_t old_value,uint64_t increment) {
    return increment>(~static_cast<uint64_t>(0u))-old_value ? (~static_cast<uint64_t>(0u)) : old_value+increment;
}
constexpr bool update_max_needed(uint64_t current,uint64_t observed) { return observed>current; }
constexpr bool snapshot_shape_valid(const Snapshot& s) {
    (void)s;
    return Count >= 20u;
}
constexpr uint64_t throughput_bytes_per_second(uint64_t bytes, uint64_t elapsed_ms) {
    if (elapsed_ms==0u) return 0u;
    const uint64_t maximum=~static_cast<uint64_t>(0u);
    if (bytes>maximum/1000u) return maximum;
    return (bytes*1000u)/elapsed_ms;
}
constexpr bool self_test() {
    if (throughput_bytes_per_second(4096u,1000u)!=4096u ||
        throughput_bytes_per_second(1u,0u)!=0u ||
        throughput_bytes_per_second(1000u,10u)!=100000u) return false;
    if (saturating_add(0u,4u)!=4u || saturating_add((~static_cast<uint64_t>(0u))-2u,4u)!=(~static_cast<uint64_t>(0u))) return false;
    if (!update_max_needed(0u,1u) || update_max_needed(8u,8u) || update_max_needed(9u,8u)) return false;
    Snapshot s{};
    for (uint32_t i=0u;i<Count;++i) s.value[i]=i;
    return snapshot_shape_valid(s) && Count>=20u;
}
} // namespace starlix::drivers::ehci::diagnostics
