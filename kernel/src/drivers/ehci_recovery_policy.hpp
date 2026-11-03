/* Phase 16/19: deterministic EHCI/MSC recovery and command serialization policy. */
#pragma once
#include "ehci_spec.hpp"

namespace starlix::drivers::ehci::recovery_policy {
enum class DeviceState : uint32_t { Ready = 0u, Recovering = 1u, Offline = 2u };
enum class BotOutcome : uint8_t { Success = 0u, CommandFailed = 1u, Recover = 2u };

/* This stack currently enumerates high-speed EHCI devices only. For a high-speed
   bulk endpoint USB 2.0 defines wMaxPacketSize as exactly 512 bytes. */
constexpr bool valid_high_speed_bulk_packet(uint16_t max_packet_size) {
    return max_packet_size == 512u;
}

constexpr bool command_admitted(bool present, DeviceState state, bool command_lock_free) {
    return present && state == DeviceState::Ready && command_lock_free;
}

constexpr bool csw_structurally_valid(uint32_t signature, uint32_t expected_signature,
                                      uint32_t tag, uint32_t expected_tag,
                                      uint32_t residue, uint32_t requested,
                                      uint8_t status) {
    return signature == expected_signature && tag == expected_tag &&
           residue <= requested && status <= 2u;
}

constexpr BotOutcome classify_bot_outcome(bool transport_ok, bool csw_valid,
                                           uint8_t csw_status, uint32_t residue,
                                           uint32_t requested, bool data_length_matches) {
    if (!transport_ok || !csw_valid || csw_status == 2u) return BotOutcome::Recover;
    if (csw_status == 1u) return BotOutcome::CommandFailed;
    if (residue != 0u || !data_length_matches || residue > requested)
        return BotOutcome::CommandFailed;
    return BotOutcome::Success;
}

constexpr DeviceState after_recovery(bool reset_ok, bool clear_in_ok, bool clear_out_ok) {
    return reset_ok && clear_in_ok && clear_out_ok ? DeviceState::Ready : DeviceState::Offline;
}

constexpr bool allow_block_io(bool present, DeviceState state, bool controller_running) {
    return present && state == DeviceState::Ready && controller_running;
}

constexpr bool self_test() {
    constexpr uint32_t CSW = 0x53425355u;
    if (!valid_high_speed_bulk_packet(512u) || valid_high_speed_bulk_packet(0u) ||
        valid_high_speed_bulk_packet(64u) || valid_high_speed_bulk_packet(513u)) return false;
    if (!command_admitted(true, DeviceState::Ready, true) ||
        command_admitted(false, DeviceState::Ready, true) ||
        command_admitted(true, DeviceState::Offline, true) ||
        command_admitted(true, DeviceState::Recovering, true) ||
        command_admitted(true, DeviceState::Ready, false)) return false;
    if (!csw_structurally_valid(CSW, CSW, 7u, 7u, 0u, 512u, 0u) ||
        csw_structurally_valid(0u, CSW, 7u, 7u, 0u, 512u, 0u) ||
        csw_structurally_valid(CSW, CSW, 8u, 7u, 0u, 512u, 0u) ||
        csw_structurally_valid(CSW, CSW, 7u, 7u, 513u, 512u, 0u) ||
        csw_structurally_valid(CSW, CSW, 7u, 7u, 0u, 512u, 3u)) return false;
    if (classify_bot_outcome(true, true, 0u, 0u, 512u, true) != BotOutcome::Success ||
        classify_bot_outcome(true, true, 1u, 0u, 512u, false) != BotOutcome::CommandFailed ||
        classify_bot_outcome(true, true, 2u, 0u, 512u, false) != BotOutcome::Recover ||
        classify_bot_outcome(false, false, 0u, 0u, 512u, false) != BotOutcome::Recover ||
        classify_bot_outcome(true, false, 0u, 0u, 512u, true) != BotOutcome::Recover ||
        classify_bot_outcome(true, true, 0u, 12u, 512u, false) != BotOutcome::CommandFailed) return false;
    if (after_recovery(true, true, true) != DeviceState::Ready ||
        after_recovery(false, true, true) != DeviceState::Offline ||
        after_recovery(true, false, true) != DeviceState::Offline ||
        after_recovery(true, true, false) != DeviceState::Offline) return false;
    if (!allow_block_io(true, DeviceState::Ready, true) ||
        allow_block_io(true, DeviceState::Offline, true) ||
        allow_block_io(true, DeviceState::Ready, false)) return false;
    return true;
}
} // namespace starlix::drivers::ehci::recovery_policy
