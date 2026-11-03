/*
 * Starlix EHCI request lifecycle policy helpers.
 * Pure admission/transition rules are shared with no-device host tests.
 */
#pragma once

namespace starlix::drivers::ehci::request_policy {

static constexpr uint32_t STATE_QUEUED = 1u;
static constexpr uint32_t STATE_ACTIVE = 2u;
static constexpr uint32_t STATE_COMPLETED = 4u;
static constexpr uint32_t STATE_FAILED = 5u;
static constexpr uint32_t STATE_CANCELLED = 6u;
static constexpr uint32_t RESULT_PENDING = 0u;
static constexpr uint32_t RESULT_SUCCESS = 1u;
static constexpr uint32_t RESULT_CANCELLED = 4u;
static constexpr uint32_t NO_TERMINAL_STATE = 0xFFFFFFFFu;

constexpr bool buffer_length_valid(uint32_t length, bool has_buffer,
                                   uint32_t maximum_length) {
    return length <= maximum_length && (length == 0u || has_buffer);
}

constexpr bool cancellation_allowed(uint32_t state) {
    return state == STATE_QUEUED;
}

constexpr bool completion_allowed(uint32_t state) {
    return state == STATE_ACTIVE;
}

constexpr bool terminal_state(uint32_t state) {
    return state == STATE_COMPLETED || state == STATE_FAILED ||
           state == STATE_CANCELLED;
}

constexpr bool terminal_result_allowed(uint32_t result) {
    return result != RESULT_PENDING && result != RESULT_CANCELLED;
}

constexpr bool completion_length_valid(uint32_t requested, uint32_t transferred) {
    /* EHCIRequestSnapshot exposes transferred as uint16_t. */
    return transferred <= requested && transferred <= 0xFFFFu;
}

constexpr uint32_t completion_state_for_result(uint32_t result) {
    if (!terminal_result_allowed(result)) return NO_TERMINAL_STATE;
    return result == RESULT_SUCCESS ? STATE_COMPLETED : STATE_FAILED;
}

inline bool self_test() {
    if (!buffer_length_valid(0u, false, 16384u) ||
        !buffer_length_valid(16384u, true, 16384u) ||
        buffer_length_valid(16385u, true, 16384u) ||
        buffer_length_valid(1u, false, 16384u)) return false;
    if (!cancellation_allowed(STATE_QUEUED) ||
        cancellation_allowed(STATE_ACTIVE) || cancellation_allowed(STATE_FAILED)) return false;
    if (!completion_allowed(STATE_ACTIVE) || completion_allowed(STATE_QUEUED) ||
        completion_allowed(STATE_COMPLETED)) return false;
    if (!terminal_state(STATE_COMPLETED) || !terminal_state(STATE_FAILED) ||
        !terminal_state(STATE_CANCELLED) || terminal_state(STATE_ACTIVE) ||
        terminal_state(STATE_QUEUED)) return false;
    if (terminal_result_allowed(RESULT_PENDING) ||
        terminal_result_allowed(RESULT_CANCELLED) ||
        !terminal_result_allowed(RESULT_SUCCESS) ||
        !terminal_result_allowed(2u) ||
        completion_state_for_result(RESULT_PENDING) != NO_TERMINAL_STATE ||
        completion_state_for_result(RESULT_CANCELLED) != NO_TERMINAL_STATE ||
        completion_state_for_result(RESULT_SUCCESS) != STATE_COMPLETED ||
        completion_state_for_result(2u) != STATE_FAILED) return false;
    if (!completion_length_valid(16u, 0u) ||
        !completion_length_valid(16u, 16u) ||
        completion_length_valid(16u, 17u) ||
        completion_length_valid(65536u, 65536u)) return false;
    return true;
}

} // namespace starlix::drivers::ehci::request_policy
