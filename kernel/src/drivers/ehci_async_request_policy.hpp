/*
 * Starlix EHCI asynchronous request execution policy.
 * Pure transition rules are shared with no-device tests.
 */
#pragma once

namespace starlix::drivers::ehci::async_request_policy {
enum : uint32_t { STAGE_IDLE = 0u, STAGE_IN_FLIGHT = 1u, STAGE_WAITING_IAA = 2u };
enum : uint32_t { POLL_PENDING = 0u, POLL_SUCCESS = 1u, POLL_ERROR = 2u };
enum : uint32_t {
    RESULT_SUCCESS = 1u, RESULT_TRANSFER_ERROR = 2u,
    RESULT_TIMED_OUT = 3u, RESULT_CANCELLED = 4u
};

constexpr bool valid_stage(uint32_t stage) { return stage <= STAGE_WAITING_IAA; }
/* Wrap-safe for intervals shorter than 2^63 milliseconds. */
constexpr uint64_t deadline_after(uint64_t now_ms, uint64_t interval_ms) {
    return now_ms + interval_ms;
}
constexpr bool deadline_reached(uint64_t now_ms, uint64_t deadline_ms) {
    return (now_ms - deadline_ms) < (1ULL << 63);
}
constexpr bool wait_can_block(bool timer_ready, bool interrupts_enabled) {
    return timer_ready && interrupts_enabled;
}
constexpr bool request_state_terminal(uint32_t state) {
    return state == 4u || state == 5u || state == 6u;
}
constexpr bool should_detach(uint32_t stage, uint32_t poll_result,
                             bool cancel_requested, bool timed_out) {
    if (stage != STAGE_IN_FLIGHT) return false;
    return cancel_requested || timed_out || poll_result == POLL_SUCCESS ||
           poll_result == POLL_ERROR;
}
constexpr uint32_t result_for_unlink(uint32_t poll_result,
                                     bool cancel_requested, bool timed_out) {
    if (cancel_requested) return RESULT_CANCELLED;
    if (timed_out) return RESULT_TIMED_OUT;
    return poll_result == POLL_SUCCESS ? RESULT_SUCCESS : RESULT_TRANSFER_ERROR;
}
constexpr bool iaa_safe_to_finish(bool status_bit, uint32_t observed_count,
                                  uint32_t baseline_count) {
    return status_bit || observed_count != baseline_count;
}
constexpr uint32_t terminal_state_for_result(uint32_t result) {
    return result == RESULT_SUCCESS ? 4u : result == RESULT_CANCELLED ? 6u : 5u;
}
constexpr bool self_test() {
    for (uint32_t stage = 0u; stage <= 2u; ++stage) {
        for (uint32_t outcome = 0u; outcome <= 2u; ++outcome) {
            for (uint32_t flags = 0u; flags < 4u; ++flags) {
                const bool cancel = (flags & 1u) != 0u;
                const bool timeout = (flags & 2u) != 0u;
                const bool expected = stage == STAGE_IN_FLIGHT &&
                    (cancel || timeout || outcome != POLL_PENDING);
                if (should_detach(stage, outcome, cancel, timeout) != expected) return false;
            }
        }
    }
    if (result_for_unlink(POLL_SUCCESS, false, false) != RESULT_SUCCESS ||
        result_for_unlink(POLL_ERROR, false, false) != RESULT_TRANSFER_ERROR ||
        result_for_unlink(POLL_SUCCESS, true, false) != RESULT_CANCELLED ||
        result_for_unlink(POLL_ERROR, false, true) != RESULT_TIMED_OUT) return false;
    if (!iaa_safe_to_finish(true, 0u, 0u) ||
        !iaa_safe_to_finish(false, 1u, 0u) ||
        iaa_safe_to_finish(false, 4u, 4u)) return false;
    if (terminal_state_for_result(RESULT_SUCCESS) != 4u ||
        terminal_state_for_result(RESULT_TRANSFER_ERROR) != 5u ||
        terminal_state_for_result(RESULT_TIMED_OUT) != 5u ||
        terminal_state_for_result(RESULT_CANCELLED) != 6u) return false;
    if (!deadline_reached(150u, 150u) || !deadline_reached(151u, 150u) ||
        deadline_reached(149u, 150u)) return false;
    const uint64_t wrapped_deadline = deadline_after(0xFFFFFFFFFFFFFFF0ULL, 32u);
    if (wrapped_deadline != 0x10ULL ||
        !deadline_reached(0x11ULL, wrapped_deadline) ||
        deadline_reached(0xFFFFFFFFFFFFFFF5ULL, wrapped_deadline)) return false;
    if (!wait_can_block(true, true) || wait_can_block(false, true) ||
        wait_can_block(true, false) || wait_can_block(false, false)) return false;
    if (!request_state_terminal(4u) || !request_state_terminal(5u) ||
        !request_state_terminal(6u) || request_state_terminal(1u) ||
        request_state_terminal(2u) || request_state_terminal(7u)) return false;
    return valid_stage(STAGE_IDLE) && valid_stage(STAGE_IN_FLIGHT) &&
           valid_stage(STAGE_WAITING_IAA) && !valid_stage(3u);
}
} // namespace starlix::drivers::ehci::async_request_policy
