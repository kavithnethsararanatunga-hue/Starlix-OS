/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once
namespace starlix::drivers::ehci::periodic {

static constexpr uint16_t MICROFRAME_BUDGET_UNITS = 6000u;
static constexpr uint16_t PROTOCOL_OVERHEAD_UNITS = 64u;
static constexpr uint32_t FRAME_COUNT = 1024u;
static constexpr uint32_t MICROFRAMES_PER_FRAME = 8u;
static constexpr uint32_t RESERVATION_CAPACITY = 16u;

struct IntervalParameters {
    uint16_t period_frames;
    uint8_t start_mask;
};

inline bool interval_parameters(uint8_t b_interval, IntervalParameters* out) {
    if (!out || b_interval == 0u || b_interval > 14u) return false;
    if (b_interval <= 4u) {
        out->period_frames = 1u;
        switch (b_interval) {
            case 1u: out->start_mask = 0xFFu; return true;
            case 2u: out->start_mask = 0x55u; return true;
            case 3u: out->start_mask = 0x11u; return true;
            case 4u: out->start_mask = 0x01u; return true;
            default: return false;
        }
    }
    out->period_frames = static_cast<uint16_t>(1u << (b_interval - 4u));
    out->start_mask = 0x01u;
    return true;
}

struct Reservation {
    bool active;
    uint32_t id;
    uint8_t device_address;
    uint8_t endpoint_address;
    uint8_t b_interval;
    uint8_t transactions;
    uint16_t max_packet_size;
    uint16_t period_frames;
    uint8_t start_mask;
    uint16_t charge_units;
};

/* Fixed storage, no allocation in the kernel or from an interrupt. Callers
   provide serialization; the public driver API wraps this object in an atomic
   bounded lock. Each microframe is budgeted in conservative byte-equivalent
   units: (payload bytes + protocol overhead) times transaction multiplier. */
class BandwidthLedger {
public:
    bool reserve(uint8_t device_address, uint8_t endpoint_address,
                 uint16_t max_packet_size, uint8_t transactions,
                 uint8_t b_interval, uint32_t* reservation_id) {
        if (!reservation_id) return false;
        *reservation_id = 0u;
        if (device_address == 0u || device_address > 127u ||
            (endpoint_address & 0x70u) != 0u ||
            (endpoint_address & 0x0Fu) == 0u ||
            max_packet_size == 0u || max_packet_size > 1024u ||
            transactions == 0u || transactions > 3u) return false;

        IntervalParameters params{};
        if (!interval_parameters(b_interval, &params)) return false;
        const uint32_t charge32 =
            (static_cast<uint32_t>(max_packet_size) + PROTOCOL_OVERHEAD_UNITS) * transactions;
        if (charge32 == 0u || charge32 > MICROFRAME_BUDGET_UNITS) return false;
        const uint16_t charge = static_cast<uint16_t>(charge32);

        Reservation* free_slot = nullptr;
        for (uint32_t i = 0; i < RESERVATION_CAPACITY; ++i) {
            if (entries_[i].active) {
                if (entries_[i].device_address == device_address &&
                    entries_[i].endpoint_address == endpoint_address) return false;
            } else if (!free_slot) {
                free_slot = &entries_[i];
            }
        }
        if (!free_slot || !fits(params, charge)) return false;

        uint32_t id = next_id_++;
        if (id == 0u) id = next_id_++;
        /* There are only 16 live IDs, but explicitly avoid wrap collisions. */
        bool collision = true;
        for (uint32_t attempt = 0; attempt <= RESERVATION_CAPACITY && collision; ++attempt) {
            collision = false;
            if (id == 0u) id = next_id_++;
            for (uint32_t i = 0; i < RESERVATION_CAPACITY; ++i) {
                if (entries_[i].active && entries_[i].id == id) { collision = true; break; }
            }
            if (collision) id = next_id_++;
        }
        if (collision || id == 0u) return false;

        apply(params, charge, true);
        free_slot->active = true;
        free_slot->id = id;
        free_slot->device_address = device_address;
        free_slot->endpoint_address = endpoint_address;
        free_slot->b_interval = b_interval;
        free_slot->transactions = transactions;
        free_slot->max_packet_size = max_packet_size;
        free_slot->period_frames = params.period_frames;
        free_slot->start_mask = params.start_mask;
        free_slot->charge_units = charge;
        *reservation_id = id;
        return true;
    }

    bool release(uint32_t reservation_id) {
        if (reservation_id == 0u) return false;
        for (uint32_t i = 0; i < RESERVATION_CAPACITY; ++i) {
            Reservation& r = entries_[i];
            if (!r.active || r.id != reservation_id) continue;
            const IntervalParameters params{r.period_frames, r.start_mask};
            if (!can_subtract(params, r.charge_units)) return false;
            apply(params, r.charge_units, false);
            r = Reservation{};
            return true;
        }
        return false;
    }

    bool usage(uint16_t frame_index, uint8_t microframe, uint16_t* used_units) const {
        if (!used_units || frame_index >= FRAME_COUNT || microframe >= MICROFRAMES_PER_FRAME)
            return false;
        *used_units = loads_[frame_index][microframe];
        return true;
    }

    uint32_t active_count() const {
        uint32_t result = 0u;
        for (uint32_t i = 0; i < RESERVATION_CAPACITY; ++i)
            if (entries_[i].active) ++result;
        return result;
    }

    void reset() {
        for (uint32_t i = 0; i < RESERVATION_CAPACITY; ++i) entries_[i] = Reservation{};
        for (uint32_t f = 0; f < FRAME_COUNT; ++f)
            for (uint32_t u = 0; u < MICROFRAMES_PER_FRAME; ++u) loads_[f][u] = 0u;
        next_id_ = 1u;
    }

private:
    Reservation entries_[RESERVATION_CAPACITY]{};
    uint16_t loads_[FRAME_COUNT][MICROFRAMES_PER_FRAME]{};
    uint32_t next_id_ = 1u;

    bool scheduled(uint32_t frame, uint32_t microframe,
                   const IntervalParameters& p) const {
        return (frame % p.period_frames) == 0u &&
               (p.start_mask & static_cast<uint8_t>(1u << microframe)) != 0u;
    }

    bool fits(const IntervalParameters& p, uint16_t charge) const {
        for (uint32_t f = 0; f < FRAME_COUNT; ++f) {
            if ((f % p.period_frames) != 0u) continue;
            for (uint32_t u = 0; u < MICROFRAMES_PER_FRAME; ++u) {
                if ((p.start_mask & static_cast<uint8_t>(1u << u)) == 0u) continue;
                if (static_cast<uint32_t>(loads_[f][u]) + charge > MICROFRAME_BUDGET_UNITS)
                    return false;
            }
        }
        return true;
    }

    bool can_subtract(const IntervalParameters& p, uint16_t charge) const {
        for (uint32_t f = 0; f < FRAME_COUNT; ++f) {
            if ((f % p.period_frames) != 0u) continue;
            for (uint32_t u = 0; u < MICROFRAMES_PER_FRAME; ++u) {
                if ((p.start_mask & static_cast<uint8_t>(1u << u)) == 0u) continue;
                if (loads_[f][u] < charge) return false;
            }
        }
        return true;
    }

    void apply(const IntervalParameters& p, uint16_t charge, bool add) {
        for (uint32_t f = 0; f < FRAME_COUNT; ++f) {
            if ((f % p.period_frames) != 0u) continue;
            for (uint32_t u = 0; u < MICROFRAMES_PER_FRAME; ++u) {
                if ((p.start_mask & static_cast<uint8_t>(1u << u)) == 0u) continue;
                if (add) loads_[f][u] = static_cast<uint16_t>(loads_[f][u] + charge);
                else loads_[f][u] = static_cast<uint16_t>(loads_[f][u] - charge);
            }
        }
    }
};

} // namespace starlix::drivers::ehci::periodic
