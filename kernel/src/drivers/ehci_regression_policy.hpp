/* Phase 30: bounded regression-suite manifest; missing suites never count as pass. */
#pragma once
#include "ehci_spec.hpp"
namespace starlix::drivers::ehci::regression_policy {
enum Suite : uint32_t { Core=1u<<0u, Transfers=1u<<1u, Ports=1u<<2u, Classes=1u<<3u,
    Storage=1u<<4u, Security=1u<<5u, Lifetime=1u<<6u, Diagnostics=1u<<7u };
static constexpr uint32_t kRequiredMask=Core|Transfers|Ports|Classes|Storage|Security|Lifetime|Diagnostics;
constexpr bool suite_id_valid(uint32_t id) { return id!=0u && (id&(id-1u))==0u && (id&kRequiredMask)!=0u; }
constexpr uint32_t record_result(uint32_t current, uint32_t suite, bool passed) {
    if (!suite_id_valid(suite) || !passed) return current;
    return current|suite;
}
constexpr bool all_required_passed(uint32_t passed_mask) { return (passed_mask&kRequiredMask)==kRequiredMask; }
constexpr bool self_test() {
    uint32_t mask=0u;
    for (uint32_t bit=1u;bit<=Diagnostics;bit<<=1u) {
        if (!suite_id_valid(bit)) return false;
        mask=record_result(mask,bit,true);
        if (all_required_passed(mask) != (mask==kRequiredMask)) return false;
    }
    if (!all_required_passed(mask) || all_required_passed(kRequiredMask&~Security)) return false;
    if (record_result(mask,3u,true)!=mask || record_result(mask,Security,false)!=mask) return false;
    return true;
}
} // namespace starlix::drivers::ehci::regression_policy
