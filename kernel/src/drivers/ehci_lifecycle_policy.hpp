/*
 * Starlix EHCI controller lifecycle policy.
 * Pure helpers keep status/admission decisions testable without MMIO.
 */
#pragma once

namespace starlix::drivers::ehci::lifecycle {

/* The public EHCIControllerState enum in ehci.hpp uses these stable numeric
   values. Keep this small policy header independent of the MMIO driver. */
static constexpr unsigned char STATE_UNINITIALIZED = 0u;
static constexpr unsigned char STATE_RUNNING = 1u;
static constexpr unsigned char STATE_STOPPED = 2u;
static constexpr unsigned char STATE_DMA_QUARANTINED = 3u;

constexpr bool status_confirms_quiescent(bool halted_acknowledged,
                                         bool async_inactive,
                                         bool periodic_inactive) {
    return halted_acknowledged && async_inactive && periodic_inactive;
}

constexpr bool status_confirms_running(bool halted_clear,
                                       bool async_schedule_active) {
    return halted_clear && async_schedule_active;
}

/* A missing MMIO view is a safe no-op only if the software does not claim the
   controller is running. Quarantine always blocks shutdown/reinitialization. */
constexpr bool shutdown_entry_allowed(bool dma_quarantined,
                                      bool operational_registers_present,
                                      bool controller_marked_active) {
    if (dma_quarantined) return false;
    if (!operational_registers_present && controller_marked_active) return false;
    return true;
}

/* The shared transfer engine must be idle, the service worker idle, and every
   request slot drained before shutdown can touch controller-owned schedules. */
constexpr bool shutdown_request_owners_clear(bool transfer_guard_acquired,
                                             bool request_worker_idle,
                                             bool all_request_slots_free) {
    return transfer_guard_acquired && request_worker_idle && all_request_slots_free;
}

/* A failed ledger lock or any live periodic reservation prevents shutdown. */
constexpr bool shutdown_periodic_ledger_clear(bool ledger_lock_acquired,
                                              unsigned int active_reservations) {
    return ledger_lock_acquired && active_reservations == 0u;
}

constexpr bool reinitialize_preflight_allowed(bool dma_quarantined) {
    return !dma_quarantined;
}

constexpr bool reinitialize_after_shutdown_allowed(bool dma_quarantined,
                                                   bool shutdown_succeeded) {
    return !dma_quarantined && shutdown_succeeded;
}

constexpr unsigned char reported_state_code(bool dma_quarantined,
                                      bool controller_active,
                                      bool controller_was_initialized) {
    if (dma_quarantined) return STATE_DMA_QUARANTINED;
    if (controller_active) return STATE_RUNNING;
    if (controller_was_initialized) return STATE_STOPPED;
    return STATE_UNINITIALIZED;
}

constexpr bool self_test() {
    /* Phase 7A: all eight quiescence combinations and RUNNING criteria. */
    for (unsigned int bits = 0; bits < 8u; ++bits) {
        const bool halted = (bits & 1u) != 0u;
        const bool async_inactive = (bits & 2u) != 0u;
        const bool periodic_inactive = (bits & 4u) != 0u;
        const bool expected_quiescent = bits == 7u;
        if (status_confirms_quiescent(halted, async_inactive, periodic_inactive) !=
            expected_quiescent) return false;
    }
    if (!status_confirms_running(true, true)) return false;
    if (status_confirms_running(false, true)) return false;
    if (status_confirms_running(true, false)) return false;
    if (status_confirms_running(false, false)) return false;

    /* Phase 7B: startup/stopped/quarantine state reporting. */
    if (reported_state_code(false, false, false) != STATE_UNINITIALIZED) return false;
    if (reported_state_code(false, true, false) != STATE_RUNNING) return false;
    if (reported_state_code(false, false, true) != STATE_STOPPED) return false;
    if (reported_state_code(true, true, true) != STATE_DMA_QUARANTINED) return false;

    /* A missing MMIO view is valid only for an inactive, non-quarantined state. */
    if (!shutdown_entry_allowed(false, false, false)) return false;
    if (shutdown_entry_allowed(false, false, true)) return false;
    if (!shutdown_entry_allowed(false, true, true)) return false;
    if (shutdown_entry_allowed(true, false, false)) return false;
    if (shutdown_entry_allowed(true, true, true)) return false;

    /* Exhaustively test owner and periodic ledger admission combinations. */
    for (unsigned int bits = 0; bits < 8u; ++bits) {
        const bool guard = (bits & 1u) != 0u;
        const bool worker_idle = (bits & 2u) != 0u;
        const bool slots_free = (bits & 4u) != 0u;
        if (shutdown_request_owners_clear(guard, worker_idle, slots_free) !=
            (bits == 7u)) return false;
    }
    if (!shutdown_periodic_ledger_clear(true, 0u)) return false;
    if (shutdown_periodic_ledger_clear(false, 0u)) return false;
    if (shutdown_periodic_ledger_clear(true, 1u)) return false;
    if (shutdown_periodic_ledger_clear(true, 0xFFFFFFFFu)) return false;

    if (!reinitialize_preflight_allowed(false)) return false;
    if (reinitialize_preflight_allowed(true)) return false;
    if (!reinitialize_after_shutdown_allowed(false, true)) return false;
    if (reinitialize_after_shutdown_allowed(false, false)) return false;
    if (reinitialize_after_shutdown_allowed(true, true)) return false;
    if (reinitialize_after_shutdown_allowed(true, false)) return false;
    return true;
}

} // namespace starlix::drivers::ehci::lifecycle
