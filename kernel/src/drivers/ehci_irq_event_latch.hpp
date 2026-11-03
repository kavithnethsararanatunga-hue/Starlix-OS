/*
 * Starlix EHCI IRQ-event latch policy.
 * Allocation-free atomic event coalescing shared by the hard IRQ producer,
 * deferred worker, and host-side regression tests.
 */
#pragma once

namespace starlix::drivers::ehci::irq {

constexpr uint32_t filter_causes(uint32_t observed_status, uint32_t allowed_mask) {
    return observed_status & allowed_mask;
}

constexpr bool generation_pending(uint32_t current, uint32_t serviced) {
    /* Inequality intentionally remains correct across uint32_t wraparound. */
    return current != serviced;
}

constexpr uint32_t next_generation(uint32_t current) {
    return current + 1u;
}

class CauseLatch {
public:
    CauseLatch() : pending_(0u), interrupt_count_(0u), auxiliary_count_(0u) {}

    /* Safe for multiple interrupt producers: causes coalesce, counters do not. */
    bool publish(uint32_t observed_status, uint32_t allowed_mask,
                 uint32_t auxiliary_mask = 0u) {
        const uint32_t accepted = filter_causes(observed_status, allowed_mask);
        if (accepted == 0u) return false;
        __atomic_fetch_or(&pending_, accepted, __ATOMIC_RELEASE);
        __atomic_fetch_add(&interrupt_count_, 1u, __ATOMIC_RELAXED);
        if ((accepted & auxiliary_mask) != 0u)
            __atomic_fetch_add(&auxiliary_count_, 1u, __ATOMIC_RELAXED);
        return true;
    }

    /* Atomically drain one coalesced cause batch. New concurrent causes remain. */
    uint32_t take_pending() {
        return __atomic_exchange_n(&pending_, 0u, __ATOMIC_ACQ_REL);
    }

    uint32_t pending_status() {
        return __atomic_load_n(&pending_, __ATOMIC_ACQUIRE);
    }

    bool has_pending() { return pending_status() != 0u; }

    uint32_t interrupt_count() {
        return __atomic_load_n(&interrupt_count_, __ATOMIC_RELAXED);
    }

    uint32_t auxiliary_count() {
        return __atomic_load_n(&auxiliary_count_, __ATOMIC_RELAXED);
    }

    void reset() {
        __atomic_store_n(&pending_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&interrupt_count_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&auxiliary_count_, 0u, __ATOMIC_RELAXED);
    }

    /* Boot-time test uses a local instance; it does not touch controller state. */
    static bool self_test() {
        CauseLatch latch;
        if (latch.publish(0x80000000u, 0x0000003Fu, 0x20u)) return false;
        if (latch.has_pending() || latch.interrupt_count() != 0u) return false;
        if (!latch.publish(0x00000001u, 0x0000003Fu, 0x20u)) return false;
        if (!latch.publish(0x00000002u, 0x0000003Fu, 0x20u)) return false;
        if (!latch.publish(0x00000022u, 0x0000003Fu, 0x20u)) return false;
        if (latch.pending_status() != 0x23u || latch.interrupt_count() != 3u ||
            latch.auxiliary_count() != 1u) return false;
        if (latch.take_pending() != 0x23u || latch.has_pending()) return false;
        if (latch.take_pending() != 0u) return false;
        if (!generation_pending(1u, 0u) || generation_pending(4u, 4u) ||
            !generation_pending(0u, 0xFFFFFFFFu) ||
            next_generation(0xFFFFFFFFu) != 0u) return false;
        latch.reset();
        return !latch.has_pending() && latch.interrupt_count() == 0u &&
               latch.auxiliary_count() == 0u;
    }

private:
    uint32_t pending_;
    uint32_t interrupt_count_;
    uint32_t auxiliary_count_;
};

} // namespace starlix::drivers::ehci::irq
