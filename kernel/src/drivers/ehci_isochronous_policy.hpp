/*
 * Starlix EHCI high-speed, one-transaction iTD policy helpers.
 * Only pure encoding/validation helpers live here; MMIO/DMA ownership stays
 * in the driver.
 */
#pragma once

namespace starlix::drivers::ehci::isochronous {

static constexpr uint16_t MAX_HIGH_SPEED_PACKET = 1024u;
static constexpr uint16_t FRAME_MASK = 0x03FFu;
static constexpr uint8_t MIN_FRAME_INTERVAL_EXPONENT = 4u;
static constexpr uint8_t MAX_FRAME_INTERVAL_EXPONENT = 10u;
static constexpr uint32_t TRANSACTION_ACTIVE = 1u << 31;
static constexpr uint32_t TRANSACTION_IOC = 1u << 15;
static constexpr uint32_t TRANSACTION_XACT_ERROR = 1u << 28;
static constexpr uint32_t TRANSACTION_BABBLE = 1u << 29;
static constexpr uint32_t TRANSACTION_BUFFER_ERROR = 1u << 30;
static constexpr uint32_t TRANSACTION_ERROR_MASK =
    TRANSACTION_XACT_ERROR | TRANSACTION_BABBLE | TRANSACTION_BUFFER_ERROR;
static constexpr uint32_t TRANSACTION_LENGTH_MASK = 0x0FFFu << 16;
static constexpr uint32_t PAGE_MASK = 0xFFFFF000u;

inline bool valid_request(uint8_t device_address, uint8_t endpoint_address,
                          uint16_t max_packet, const void* buffer,
                          uint16_t length) {
    return device_address != 0u && device_address <= 127u &&
           (endpoint_address & 0x70u) == 0u && (endpoint_address & 0x0Fu) != 0u &&
           max_packet != 0u && max_packet <= MAX_HIGH_SPEED_PACKET &&
           length != 0u && length <= max_packet && buffer != nullptr;
}

inline bool interval_period_frames(uint8_t b_interval, uint16_t* out_period) {
    if (!out_period || b_interval < MIN_FRAME_INTERVAL_EXPONENT ||
        b_interval > MAX_FRAME_INTERVAL_EXPONENT) return false;
    *out_period = static_cast<uint16_t>(1u << (b_interval - MIN_FRAME_INTERVAL_EXPONENT));
    return true;
}

inline uint16_t next_frame(uint16_t current_frame, uint16_t lead_frames = 2u) {
    return static_cast<uint16_t>((current_frame + lead_frames) & FRAME_MASK);
}

inline bool aligned_target_frame(uint16_t current_frame, uint16_t period_frames,
                                 uint16_t* target_frame, uint16_t* frame_delta) {
    if (!target_frame || !frame_delta || period_frames == 0u ||
        period_frames > 64u || (period_frames & (period_frames - 1u)) != 0u)
        return false;
    uint16_t frame = next_frame(current_frame, 2u);
    uint16_t delta = static_cast<uint16_t>((frame - current_frame) & FRAME_MASK);
    while ((frame % period_frames) != 0u) {
        frame = static_cast<uint16_t>((frame + 1u) & FRAME_MASK);
        ++delta;
        if (delta >= 1024u) return false;
    }
    *target_frame = frame;
    *frame_delta = delta;
    return true;
}

inline uint32_t transaction_token(uint16_t requested_bytes) {
    return TRANSACTION_ACTIVE | TRANSACTION_IOC |
           (static_cast<uint32_t>(requested_bytes) << 16);
}

inline uint32_t page0_pointer(uint32_t page_address, uint8_t device_address,
                              uint8_t endpoint_address) {
    return (page_address & PAGE_MASK) |
           (static_cast<uint32_t>(device_address) & 0x7Fu) |
           (static_cast<uint32_t>(endpoint_address & 0x0Fu) << 8);
}

inline uint32_t page1_pointer(uint32_t page_address, uint16_t max_packet,
                              bool data_in) {
    return (page_address & PAGE_MASK) |
           (static_cast<uint32_t>(max_packet) & 0x07FFu) |
           (data_in ? (1u << 11) : 0u);
}

inline uint32_t page2_pointer(uint32_t page_address, uint8_t transactions = 1u) {
    return (page_address & PAGE_MASK) | (static_cast<uint32_t>(transactions) & 0x3u);
}

inline bool transaction_active(uint32_t token) {
    return (token & TRANSACTION_ACTIVE) != 0u;
}

inline bool transaction_failed(uint32_t token) {
    return (token & TRANSACTION_ERROR_MASK) != 0u;
}

/* EHCI iTD semantics differ by direction: for IN, the controller writes the
   number of bytes actually received into the transaction Length field. For
   OUT, software must not assume hardware rewrites Length; a successful token
   therefore reports the requested length. */
inline bool actual_length(uint32_t token, bool data_in, uint16_t requested,
                          uint16_t* out) {
    if (!out) return false;
    if (!data_in) {
        if ((token & TRANSACTION_ACTIVE) != 0u || transaction_failed(token)) return false;
        *out = requested;
        return true;
    }
    const uint32_t received = (token & TRANSACTION_LENGTH_MASK) >> 16;
    if (received > requested) return false;
    *out = static_cast<uint16_t>(received);
    return true;
}

inline bool self_test() {
    if (!valid_request(1u, 0x81u, 1024u, reinterpret_cast<const void*>(1u), 1024u)) return false;
    if (valid_request(0u, 0x81u, 64u, reinterpret_cast<const void*>(1u), 1u)) return false;
    if (valid_request(1u, 0x80u, 64u, reinterpret_cast<const void*>(1u), 1u)) return false;
    if (valid_request(1u, 0x81u, 1025u, reinterpret_cast<const void*>(1u), 1u)) return false;
    if (valid_request(1u, 0x81u, 64u, nullptr, 1u)) return false;
    if (next_frame(1022u) != 0u || next_frame(10u, 1u) != 11u) return false;
    uint16_t interval = 0u, target = 0u, delta = 0u;
    if (!interval_period_frames(4u, &interval) || interval != 1u) return false;
    if (!interval_period_frames(10u, &interval) || interval != 64u) return false;
    if (interval_period_frames(3u, &interval) || interval_period_frames(11u, &interval)) return false;
    if (!aligned_target_frame(10u, 4u, &target, &delta) || target != 12u || delta != 2u) return false;
    if (!aligned_target_frame(10u, 8u, &target, &delta) || target != 16u || delta != 6u) return false;
    if (!transaction_active(transaction_token(64u))) return false;
    if (transaction_failed(transaction_token(64u))) return false;
    uint16_t actual = 0u;
    /* For IN, Length contains received bytes on completion, not bytes remaining. */
    if (!actual_length(0u, true, 64u, &actual) || actual != 0u) return false;
    const uint32_t short_in_token = (32u << 16);
    if (!actual_length(short_in_token, true, 64u, &actual) || actual != 32u) return false;
    if (actual_length((65u << 16), true, 64u, &actual)) return false;
    /* The EHCI controller need not rewrite the OUT Length field. */
    if (!actual_length(0u, false, 64u, &actual) || actual != 64u) return false;
    if (actual_length(TRANSACTION_ACTIVE, false, 64u, &actual)) return false;
    if (actual_length(TRANSACTION_XACT_ERROR, false, 64u, &actual)) return false;
    if (!transaction_failed(TRANSACTION_XACT_ERROR) ||
        !transaction_failed(TRANSACTION_BABBLE) ||
        !transaction_failed(TRANSACTION_BUFFER_ERROR)) return false;
    if ((page0_pointer(0x12345000u, 7u, 0x83u) & PAGE_MASK) != 0x12345000u) return false;
    if ((page1_pointer(0x12345000u, 512u, true) & (1u << 11)) == 0u) return false;
    if ((page2_pointer(0x12345000u, 1u) & 0x3u) != 1u) return false;
    return true;
}

} // namespace starlix::drivers::ehci::isochronous
