/*
 * Starlix EHCI deferred IRQ dispatch policy.
 * Pure mapping from latched USBSTS causes to non-IRQ worker actions.
 */
#pragma once
#include "ehci_spec.hpp"

namespace starlix::drivers::ehci::irq {

struct DeferredPlan {
    uint32_t recognized_causes;
    bool harvest_transfer_completions;
    bool service_root_port_changes;
    bool record_controller_error;
    bool observe_async_advance;

    constexpr bool has_work() const { return recognized_causes != 0u; }
};

constexpr DeferredPlan classify_deferred_causes(uint32_t status) {
    const uint32_t recognized = status &
        (STS_INT | STS_ERR | STS_PORT_CHANGE | STS_HOST_ERROR | STS_ASYNC_ADVANCE);
    const bool transfer_error = (recognized & (STS_ERR | STS_HOST_ERROR)) != 0u;
    return {
        recognized,
        (recognized & STS_INT) != 0u || transfer_error,
        (recognized & STS_PORT_CHANGE) != 0u,
        transfer_error,
        (recognized & STS_ASYNC_ADVANCE) != 0u
    };
}

/* Exhaust all combinations of the five enabled cause bits. This is a pure
   policy test: it does not touch MMIO and requires no enumerated device. */
constexpr bool deferred_dispatch_self_test() {
    constexpr uint32_t causes[5] = {
        STS_INT, STS_ERR, STS_PORT_CHANGE, STS_HOST_ERROR, STS_ASYNC_ADVANCE
    };
    for (uint32_t bits = 0u; bits < 32u; ++bits) {
        uint32_t status = 0u;
        for (uint32_t i = 0u; i < 5u; ++i)
            if ((bits & (1u << i)) != 0u) status |= causes[i];
        const DeferredPlan plan = classify_deferred_causes(status);
        const bool expected_error = (status & (STS_ERR | STS_HOST_ERROR)) != 0u;
        if (plan.recognized_causes != status || plan.has_work() != (status != 0u)) return false;
        if (plan.harvest_transfer_completions !=
            (((status & STS_INT) != 0u) || expected_error)) return false;
        if (plan.service_root_port_changes != ((status & STS_PORT_CHANGE) != 0u)) return false;
        if (plan.record_controller_error != expected_error) return false;
        if (plan.observe_async_advance != ((status & STS_ASYNC_ADVANCE) != 0u)) return false;
    }
    const DeferredPlan empty = classify_deferred_causes(0u);
    if (empty.has_work() || empty.harvest_transfer_completions ||
        empty.service_root_port_changes || empty.record_controller_error ||
        empty.observe_async_advance) return false;
    const DeferredPlan unrecognized = classify_deferred_causes(STS_FRAME_ROLLOVER | 0x80000000u);
    if (unrecognized.has_work() || unrecognized.recognized_causes != 0u) return false;
    return true;
}

} // namespace starlix::drivers::ehci::irq
