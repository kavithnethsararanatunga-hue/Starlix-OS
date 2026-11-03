/* Phase 33: bounded USB HID boot-input event queue and report-delta policy. */
#pragma once
#include "ehci_hid_policy.hpp"

namespace starlix::drivers::ehci::hid_input {

enum class EventType : uint8_t { KeyDown = 1u, KeyUp = 2u, MouseReport = 3u };

struct Event {
    EventType type;
    uint8_t code;       /* HID usage for key events; 0 for mouse reports. */
    uint8_t modifiers;  /* Boot-keyboard modifier bitmap. */
    uint8_t buttons;    /* Mouse buttons, bits 0..2. */
    int16_t dx;
    int16_t dy;
    int8_t wheel;
};

static constexpr uint32_t EVENT_QUEUE_CAPACITY = 128u;
static constexpr uint32_t BOOT_INTERFACE_CAPACITY = 8u;

/* Periodic interrupt QHs complete in hardware even when PCI INTx cannot be
   routed to the legacy PIC. HID harvesting must therefore depend on the
   controller being active, not on the presence of an IRQ callback. */
constexpr bool service_enabled(bool controller_active, bool irq_available) {
    (void)irq_available;
    return controller_active;
}

/* Root-port change bits are W1C status and can be polled directly when no IRQ
   route exists. Callers must acknowledge and rescan in normal context. */
constexpr bool port_change_pending(uint32_t port_status, uint32_t change_mask) {
    return (port_status & change_mask) != 0u;
}

struct BootInterfaceCandidate {
    bool active;
    bool keyboard;
    bool endpoint_found;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
};

/* Finds only boot-protocol HID interfaces with one usable interrupt-IN
   endpoint compatible with the current EHCI high-speed periodic scheduler.
   This parser is pure and bounded so descriptor edge cases can be host-tested. */
inline bool parse_boot_interfaces(const uint8_t* descriptors, uint16_t total_length,
                                  BootInterfaceCandidate* out, uint32_t capacity,
                                  uint32_t* out_count, uint8_t max_interval = 14u,
                                  uint8_t min_interval = 1u) {
    if (out_count) *out_count = 0u;
    if (!descriptors || !out || !out_count || capacity == 0u ||
        total_length < 9u || descriptors[0] < 9u ||
        descriptors[1] != USB_DESC_CONFIGURATION) return false;
    const uint16_t declared = static_cast<uint16_t>(descriptors[2] |
        (static_cast<uint16_t>(descriptors[3]) << 8u));
    if (declared != total_length || descriptors[0] > total_length) return false;
    for (uint32_t i = 0u; i < capacity; ++i) out[i] = BootInterfaceCandidate{};

    int32_t current = -1;
    uint16_t offset = descriptors[0];
    while (offset + 2u <= total_length) {
        const uint8_t length = descriptors[offset];
        const uint8_t type = descriptors[offset + 1u];
        if (length < 2u || static_cast<uint32_t>(offset) + length > total_length)
            return false;
        if (type == USB_DESC_INTERFACE) {
            current = -1;
            if (length < 9u) return false;
            if (descriptors[offset + 3u] == 0u && descriptors[offset + 5u] == 0x03u &&
                descriptors[offset + 6u] == 1u &&
                (descriptors[offset + 7u] == 1u || descriptors[offset + 7u] == 2u)) {
                uint32_t slot = 0u;
                while (slot < *out_count &&
                       out[slot].interface_number != descriptors[offset + 2u]) ++slot;
                if (slot == *out_count) {
                    if (*out_count >= capacity) {
                        current = -1;
                        offset = static_cast<uint16_t>(offset + length);
                        continue;
                    }
                    ++(*out_count);
                }
                out[slot].active = true;
                out[slot].keyboard = descriptors[offset + 7u] == 1u;
                out[slot].interface_number = descriptors[offset + 2u];
                current = static_cast<int32_t>(slot);
            }
        } else if (type == USB_DESC_ENDPOINT && current >= 0 && length >= 7u) {
            BootInterfaceCandidate& candidate = out[static_cast<uint32_t>(current)];
            const uint8_t address = descriptors[offset + 2u];
            const uint8_t attributes = descriptors[offset + 3u];
            const uint16_t raw_packet = static_cast<uint16_t>(descriptors[offset + 4u] |
                (static_cast<uint16_t>(descriptors[offset + 5u]) << 8u));
            const uint16_t packet = static_cast<uint16_t>(raw_packet & 0x07FFu);
            const uint8_t interval = descriptors[offset + 6u];
            const uint16_t minimum = candidate.keyboard ? 8u : 3u;
            if (!candidate.endpoint_found && (attributes & 0x03u) == 0x03u &&
                (address & 0x80u) != 0u && (address & 0x70u) == 0u &&
                (address & 0x0Fu) != 0u && (raw_packet & 0xF800u) == 0u &&
                packet >= minimum && packet <= 64u && interval >= min_interval && interval <= max_interval) {
                candidate.endpoint_found = true;
                candidate.endpoint_address = address;
                candidate.interval = interval;
                candidate.max_packet_size = packet;
            }
        }
        offset = static_cast<uint16_t>(offset + length);
    }
    return true;
}

/* Fixed-capacity, non-allocating queue. It is serviced only in normal kernel
   context; hard-IRQ callbacks never publish HID events into this queue. */
class EventQueue {
public:
    bool push(const Event& event) {
        if (count_ == EVENT_QUEUE_CAPACITY) {
            if (dropped_ != 0xFFFFFFFFu) ++dropped_;
            return false;
        }
        entries_[tail_] = event;
        tail_ = (tail_ + 1u) % EVENT_QUEUE_CAPACITY;
        ++count_;
        return true;
    }

    bool pop(Event* out) {
        if (!out || count_ == 0u) return false;
        *out = entries_[head_];
        head_ = (head_ + 1u) % EVENT_QUEUE_CAPACITY;
        --count_;
        return true;
    }

    void clear() {
        head_ = 0u;
        tail_ = 0u;
        count_ = 0u;
        dropped_ = 0u;
    }

    uint32_t size() const { return count_; }
    uint32_t dropped() const { return dropped_; }

private:
    Event entries_[EVENT_QUEUE_CAPACITY]{};
    uint32_t head_ = 0u;
    uint32_t tail_ = 0u;
    uint32_t count_ = 0u;
    uint32_t dropped_ = 0u;
};

inline bool key_present(const uint8_t keys[6], uint8_t usage) {
    if (!keys || usage == 0u) return false;
    for (uint32_t i = 0u; i < 6u; ++i)
        if (keys[i] == usage) return true;
    return false;
}

inline bool emit_key(EventQueue* queue, EventType type, uint8_t usage,
                     uint8_t modifiers) {
    if (!queue || usage == 0u || usage <= 3u) return false;
    Event event{};
    event.type = type;
    event.code = usage;
    event.modifiers = modifiers;
    return queue->push(event);
}

/* Emits HID usage transitions, rather than repeating a key-down on every
   polling interval. Usage 1..3 is reserved for rollover/error reports and is
   filtered by decode_boot_keyboard before this helper should be called. */
inline bool keyboard_report_delta(const hid_policy::BootKeyboardReport* previous,
                                  const hid_policy::BootKeyboardReport* current,
                                  EventQueue* queue) {
    if (!previous || !current || !queue) return false;
    bool complete = true;
    for (uint8_t bit = 0u; bit < 8u; ++bit) {
        const uint8_t mask = static_cast<uint8_t>(1u << bit);
        if ((previous->modifiers & mask) == (current->modifiers & mask)) continue;
        const EventType type = (current->modifiers & mask) ? EventType::KeyDown : EventType::KeyUp;
        if (!emit_key(queue, type, static_cast<uint8_t>(0xE0u + bit), current->modifiers))
            complete = false;
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        const uint8_t usage = previous->keys[i];
        if (usage >= 4u && !key_present(current->keys, usage) &&
            !emit_key(queue, EventType::KeyUp, usage, current->modifiers)) complete = false;
    }
    for (uint32_t i = 0u; i < 6u; ++i) {
        const uint8_t usage = current->keys[i];
        if (usage >= 4u && !key_present(previous->keys, usage) &&
            !emit_key(queue, EventType::KeyDown, usage, current->modifiers)) complete = false;
    }
    return complete;
}

inline bool mouse_report_delta(const hid_policy::BootMouseReport* previous,
                               const hid_policy::BootMouseReport* current,
                               EventQueue* queue) {
    if (!previous || !current || !queue) return false;
    if (current->dx == 0 && current->dy == 0 && current->wheel == 0 &&
        previous->buttons == current->buttons) return true;
    Event event{};
    event.type = EventType::MouseReport;
    event.buttons = current->buttons;
    event.dx = current->dx;
    event.dy = current->dy;
    event.wheel = current->wheel;
    return queue->push(event);
}

inline bool self_test() {
    if (service_enabled(false, false) || service_enabled(false, true) ||
        !service_enabled(true, false) || !service_enabled(true, true)) return false;
    if (!port_change_pending(1u, 1u) || port_change_pending(1u, 2u) ||
        !port_change_pending(3u, 2u)) return false;

    EventQueue queue{};
    hid_policy::BootKeyboardReport empty{};
    hid_policy::BootKeyboardReport pressed{};
    pressed.modifiers = 0x02u; /* left Shift, HID usage 0xE1 */
    pressed.keys[0] = 0x04u;   /* HID usage for A */
    if (!keyboard_report_delta(&empty, &pressed, &queue) || queue.size() != 2u) return false;
    Event event{};
    if (!queue.pop(&event) || event.type != EventType::KeyDown || event.code != 0xE1u) return false;
    if (!queue.pop(&event) || event.type != EventType::KeyDown || event.code != 0x04u) return false;
    if (!keyboard_report_delta(&pressed, &pressed, &queue) || queue.size() != 0u) return false;
    if (!keyboard_report_delta(&pressed, &empty, &queue) || queue.size() != 2u) return false;
    if (!queue.pop(&event) || event.type != EventType::KeyUp || event.code != 0xE1u) return false;
    if (!queue.pop(&event) || event.type != EventType::KeyUp || event.code != 0x04u) return false;

    hid_policy::BootMouseReport mouse_old{};
    hid_policy::BootMouseReport mouse_new{};
    mouse_new.buttons = 1u; mouse_new.dx = -3; mouse_new.dy = 4; mouse_new.wheel = 1;
    if (!mouse_report_delta(&mouse_old, &mouse_new, &queue) || !queue.pop(&event) ||
        event.type != EventType::MouseReport || event.buttons != 1u ||
        event.dx != -3 || event.dy != 4 || event.wheel != 1) return false;

    const uint8_t config[] = {9,2,25,0,1,1,0,0x80,25,
        9,4,0,0,1,3,1,1,0, 7,5,0x81,3,8,0,10};
    BootInterfaceCandidate candidates[BOOT_INTERFACE_CAPACITY]{};
    uint32_t candidate_count = 0u;
    if (!parse_boot_interfaces(config, sizeof(config), candidates,
            BOOT_INTERFACE_CAPACITY, &candidate_count) || candidate_count != 1u ||
        !candidates[0].active || !candidates[0].keyboard ||
        !candidates[0].endpoint_found || candidates[0].interface_number != 0u ||
        candidates[0].endpoint_address != 0x81u || candidates[0].interval != 10u ||
        candidates[0].max_packet_size != 8u) return false;
    uint8_t bad_interval[sizeof(config)]{};
    for (uint32_t i = 0u; i < sizeof(config); ++i) bad_interval[i] = config[i];
    bad_interval[24] = 15u;
    candidate_count = 0u;
    if (!parse_boot_interfaces(bad_interval, sizeof(bad_interval), candidates,
            BOOT_INTERFACE_CAPACITY, &candidate_count) || candidate_count != 1u ||
        candidates[0].endpoint_found) return false;
    uint8_t full_speed_interval[sizeof(config)]{};
    for (uint32_t i = 0u; i < sizeof(config); ++i) full_speed_interval[i] = config[i];
    full_speed_interval[24] = 255u;
    if (!parse_boot_interfaces(full_speed_interval, sizeof(full_speed_interval), candidates,
            BOOT_INTERFACE_CAPACITY, &candidate_count, 255u, 1u) ||
        !candidates[0].endpoint_found || candidates[0].interval != 255u) return false;
    full_speed_interval[24] = 9u;
    if (!parse_boot_interfaces(full_speed_interval, sizeof(full_speed_interval), candidates,
            BOOT_INTERFACE_CAPACITY, &candidate_count, 255u, 10u) ||
        candidates[0].endpoint_found) return false;
    full_speed_interval[24] = 10u;
    if (!parse_boot_interfaces(full_speed_interval, sizeof(full_speed_interval), candidates,
            BOOT_INTERFACE_CAPACITY, &candidate_count, 255u, 10u) ||
        !candidates[0].endpoint_found) return false;
    if (parse_boot_interfaces(config, sizeof(config), nullptr,
            BOOT_INTERFACE_CAPACITY, &candidate_count)) return false;

    for (uint32_t i = 0u; i < EVENT_QUEUE_CAPACITY; ++i)
        if (!queue.push(Event{})) return false;
    if (queue.push(Event{}) || queue.size() != EVENT_QUEUE_CAPACITY || queue.dropped() != 1u)
        return false;
    queue.clear();
    return queue.size() == 0u && queue.dropped() == 0u && !queue.pop(&event);
}

} // namespace starlix::drivers::ehci::hid_input
