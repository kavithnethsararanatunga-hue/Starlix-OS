/*
 * Starlix EHCI bounded periodic QH schedule helpers.
 * Fixed-capacity, allocation-free algorithms shared with host tests.
 */
#pragma once


namespace starlix::drivers::ehci::periodic_schedule {

static constexpr uint32_t FRAME_COUNT = 1024u;
static constexpr uint8_t NO_NODE = 0xFFu;

inline bool valid_period(uint16_t period_frames) {
    return period_frames != 0u && period_frames <= FRAME_COUNT &&
           (period_frames & static_cast<uint16_t>(period_frames - 1u)) == 0u;
}

/* Long-interval QHs must precede short-interval QHs. EHCI frame-list entries
   then point at the longest-period QH due in that frame; the linked suffix is
   exactly the set of shorter power-of-two intervals due in the same frame. */
inline bool build_order(const uint16_t* periods, const bool* active,
                        uint32_t count, uint8_t* order, uint32_t* out_count) {
    if (!periods || !active || !order || !out_count || count > 32u) return false;
    *out_count = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        if (!active[i]) continue;
        if (!valid_period(periods[i])) return false;
        uint32_t at = *out_count;
        while (at > 0u && periods[order[at - 1u]] < periods[i]) {
            order[at] = order[at - 1u];
            --at;
        }
        order[at] = static_cast<uint8_t>(i);
        ++(*out_count);
    }
    return true;
}

inline uint8_t frame_head(uint16_t frame, const uint16_t* periods,
                          const uint8_t* order, uint32_t order_count) {
    if (!periods || !order || frame >= FRAME_COUNT) return NO_NODE;
    for (uint32_t p = 0; p < order_count; ++p) {
        const uint8_t index = order[p];
        if (valid_period(periods[index]) && (frame % periods[index]) == 0u)
            return index;
    }
    return NO_NODE;
}

} // namespace starlix::drivers::ehci::periodic_schedule
