/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once
#include "pci.hpp"
#include "ehci_spec.hpp"
#include "ehci_periodic_schedule.hpp"
#include "ehci_isochronous_policy.hpp"
#include "ehci_diagnostics_policy.hpp"
#include "ehci_release_policy.hpp"
#include "ehci_hid_input_policy.hpp"

namespace starlix::drivers::ehci {
struct MSCDeviceInfo {
    uint32_t last_lba;
    uint32_t block_size;
};

/* Phase 18: public snapshot from the bounded USB device model. */
enum class EHCIUSBDriverBinding : uint8_t {
    Unbound = 0, HubManagement = 1, MassStorageBOT = 2,
    VendorGenericAPI = 3, HIDInput = 4
};
struct EHCIUSBDeviceInfo {
    uint8_t device_address;
    uint8_t root_port_number; /* one-based */
    uint8_t lifecycle_state;
    uint8_t driver_binding;
    uint32_t generation;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usb_version_bcd;
    uint16_t device_version_bcd;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint8_t interface_count;
    uint16_t endpoint_count;
};

/* Phase 33: kernel USB HID input events after decoding boot-protocol reports. */
using EHCIInputEventType = starlix::drivers::ehci::hid_input::EventType;
using EHCIInputEvent = starlix::drivers::ehci::hid_input::Event;
bool ehci_hid_service();
bool ehci_hid_event_poll(EHCIInputEvent* out);
uint32_t ehci_hid_active_count();
uint32_t ehci_hid_dropped_event_count();

/* Controller lifecycle state. DmaQuarantined means a halt/schedule-stop failure
   occurred; do not reuse its DMA pages until reboot. */
enum class EHCIControllerState : uint8_t {
    Uninitialized = 0,
    Running = 1,
    Stopped = 2,
    DmaQuarantined = 3
};

/* Phase 8: atomically consume latched EHCI causes and read cumulative counters.
   pending_status is drained; the two counters are cumulative snapshots. */
struct EHCIIRQEvents {
    uint32_t pending_status;
    uint32_t interrupt_count;
    uint32_t async_advance_count;
};

/* Phase 31: monotonic runtime diagnostics snapshot. Values saturate instead of
   wrapping; this can be read without resetting counters or disturbing USB I/O. */
bool ehci_diagnostics_snapshot(starlix::drivers::ehci::diagnostics::Snapshot* out);

/* Phase 9: bounded request descriptors and lifecycle states. Submission is
   nonblocking; a single hardware QH/qTD chain is advanced by deferred service. */
enum class EHCIRequestType : uint8_t {
    Control = 1,
    Bulk = 2
};

enum class EHCIRequestState : uint32_t {
    Free = 0,
    Queued = 1,
    Active = 2,
    Completing = 3,
    Completed = 4,
    Failed = 5,
    Cancelled = 6,
    Reserving = 7
};

enum class EHCIRequestResult : uint32_t {
    Pending = 0,
    Success = 1,
    TransferError = 2,
    TimedOut = 3,
    Cancelled = 4
};

static constexpr uint32_t EHCI_REQUEST_CAPACITY = 32;

/* data_buffer must remain valid until the request is released. Control setup
   data is copied into the descriptor; the pointed-to data buffer is not. */
struct EHCITransferRequest {
    EHCIRequestType type;
    uint8_t device_address;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    USB_SETUP_PACKET setup;
    void* data_buffer;
    uint16_t data_length;
};

/* dequeue() returns the descriptor and marks it Active. */
struct EHCIQueuedRequest {
    uint32_t request_id;
    EHCITransferRequest transfer;
    EHCIRequestState state;
};

/* poll() is a nonblocking status snapshot. Terminal requests must be released. */
struct EHCIRequestSnapshot {
    uint32_t request_id;
    EHCIRequestState state;
    EHCIRequestResult result;
    uint16_t transferred;
};

/* Initializes the fixed-capacity request table once; safe to call repeatedly. */
bool ehci_request_queue_init();
/* Rejects invalid descriptors and returns false when all 32 slots are occupied. */
bool ehci_request_submit(const EHCITransferRequest* request, uint32_t* request_id);
/* Claims the oldest queued request and changes it to Active; service_one uses this
   internally before linking the request QH to the asynchronous schedule. */
bool ehci_request_dequeue(EHCIQueuedRequest* out);
/* Completes a software-owned Active request. The active hardware request is
   completed only by the deferred engine after its async-advance safe point. */
bool ehci_request_complete(uint32_t request_id, EHCIRequestResult result,
                           uint16_t transferred);
/* Cancels queued requests immediately; active requests request a safe QH unlink
   and complete cancellation only after the async-advance safe point. */
bool ehci_request_cancel(uint32_t request_id);
/* Returns the current state/result without blocking. */
bool ehci_request_poll(uint32_t request_id, EHCIRequestSnapshot* out);
/* Timer-backed wait for a terminal result. Call only from non-IRQ context with
   IF enabled. True means terminal (inspect out->result); false means timeout,
   invalid request, or unavailable clock/interrupts. A timed-out wait does NOT
   cancel the request, so the data buffer must stay valid until terminal/release. */
bool ehci_request_wait(uint32_t request_id, uint32_t timeout_ms,
                       EHCIRequestSnapshot* out);
/* Frees only a terminal request. Invalidates its request_id. */
bool ehci_request_release(uint32_t request_id);

/*
 * Advances the single nonblocking control/bulk request engine from non-IRQ
 * context. A submitted request returns immediately; this worker attaches its QH,
 * reaps completion after IAA, or unlinks on timeout/cancellation. The user buffer
 * must remain valid until a terminal snapshot is polled and the ID is released.
 */
bool ehci_request_service_one();

/* Returns false for a null output pointer. Safe for a non-IRQ caller. */
bool ehci_irq_take_events(EHCIIRQEvents* out);

/* Read-only metadata for an enumerated USB Mass Storage device. */
bool ehci_msc_get_device_info(uint8_t dev_addr, MSCDeviceInfo* out);
/* Phase 18: device-model snapshot is valid only for a configured, current generation. */
bool ehci_usb_get_device_info(uint8_t dev_addr, EHCIUSBDeviceInfo* out);


/* Initializes the PCI EHCI controller and probes high-speed root-port devices. */
bool ehci_init();

/* Read current controller lifecycle state. These APIs are non-IRQ and must be
   called from a serialized system-control context after quiescing USB clients.
   shutdown retains DMA pages; call ehci_init() or reinitialize() to restart. */
EHCIControllerState ehci_controller_state();
bool ehci_controller_shutdown();
bool ehci_controller_reinitialize();

/* Phase 17: conservative root-port power management. Port index is zero-based.
   Suspend refuses while requests, periodic endpoints or the transfer engine are busy. */
bool ehci_root_port_suspend(uint8_t port_index);
bool ehci_root_port_resume(uint8_t port_index);

/* Synchronous USB control transfer on endpoint zero. Up to 16 KiB via bounded qTD chain. */
bool ehci_control_transfer(uint8_t dev_addr, USB_SETUP_PACKET* setup, void* data_buf, uint16_t data_len);

/* Synchronous, polled high-speed bulk transfer; transfer size is capped at 16 KiB via qTD chains. */
bool ehci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint_address,
                        uint16_t max_packet, void* data_buf, uint16_t data_len,
                        uint16_t* transferred);

/* Legacy one-shot high-speed interrupt endpoint request. bInterval must be 1..14;
   max_packet/data_len are limited to one packet (<=1024 bytes). It remains
   synchronous bounded polling. Use ehci_periodic_interrupt_open/poll/close for
   independent persistent interrupt-IN endpoints (fixed capacity). */
bool ehci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint_address,
                             uint16_t max_packet, uint8_t b_interval,
                             void* data_buf, uint16_t data_len,
                             uint16_t* transferred);

/* Phase 10: high-speed periodic bandwidth admission accounting. The persistent
   interrupt endpoint manager uses these reservations before attaching QHs to
   the live frame-list schedule. Charge units conservatively estimate payload
   plus protocol overhead per scheduled microframe. */
struct EHCIPeriodicBandwidthStatus {
    uint16_t frame_index;       /* 0..1023 in the current periodic frame-list horizon */
    uint8_t microframe;         /* 0..7 */
    uint16_t used_units;
    uint16_t budget_units;
};
static constexpr uint16_t EHCI_PERIODIC_MICROFRAME_BUDGET_UNITS = 6000u;
bool ehci_periodic_bandwidth_reserve(uint8_t device_address, uint8_t endpoint_address,
                                     uint16_t max_packet_size, uint8_t transactions,
                                     uint8_t b_interval, uint32_t* reservation_id);
bool ehci_periodic_bandwidth_release(uint32_t reservation_id);
bool ehci_periodic_bandwidth_query(uint16_t frame_index, uint8_t microframe,
                                   EHCIPeriodicBandwidthStatus* out);

/* Phase 10: persistent interrupt-IN endpoint schedules. Call from serialized,
   non-IRQ context. The endpoint remains linked until close; poll is nonblocking.
   Each slot has one outstanding transaction, rearmed after the completed sample
   is consumed. Capacity is fixed, and DMA pages remain boot-lifetime allocated. */
static constexpr uint32_t EHCI_PERIODIC_ENDPOINT_CAPACITY = 8u;
enum class EHCIPeriodicPollResult : uint8_t {
    NoData = 0, Completed = 1, TransferError = 2, InvalidEndpoint = 3, BufferTooSmall = 4
};
bool ehci_periodic_interrupt_open(uint8_t device_address, uint8_t endpoint_address,
                                  uint16_t max_packet_size, uint8_t b_interval,
                                  uint32_t* endpoint_id);
EHCIPeriodicPollResult ehci_periodic_interrupt_poll(uint32_t endpoint_id,
                                                    void* out_buffer, uint16_t capacity,
                                                    uint16_t* transferred);
bool ehci_periodic_interrupt_close(uint32_t endpoint_id);
uint32_t ehci_periodic_interrupt_active_count();

/* Phase 11: one-shot high-speed isochronous transfer, one transaction of at most
   1024 bytes in microframe zero. bInterval exponents 4..10 (one to 64 frames)
   are supported by the current 1024-entry frame-list implementation. Split/TT,
   sub-frame intervals, multi-transaction payloads and streaming remain unsupported. */
enum class EHCIIsochronousResult : uint8_t {
    Success = 0, InvalidRequest = 1, Busy = 2, DmaFailure = 3,
    ScheduleFailure = 4, Timeout = 5, TransactionError = 6,
    MissedMicroframe = 7, BufferError = 8
};
struct EHCIIsochronousStats {
    uint32_t attempts;
    uint32_t completed;
    uint32_t timeouts;
    uint32_t transaction_errors;
    uint32_t missed_microframes;
    uint32_t buffer_errors;
};
EHCIIsochronousResult ehci_isochronous_transfer(uint8_t device_address,
                                                uint8_t endpoint_address,
                                                uint8_t b_interval,
                                                uint16_t max_packet_size,
                                                void* data_buffer, uint16_t data_length,
                                                uint16_t* transferred);
void ehci_isochronous_get_stats(EHCIIsochronousStats* out);

/* Deferred non-IRQ service. Drains interrupt causes, reaps one active async
   control/bulk request, marks periodic completions and starts queued requests. */
bool ehci_service_deferred_work();
bool ehci_deferred_work_pending();

/* USB MSC BOT block I/O. Storage requests remain capped at 4096 bytes and synchronous. */
bool ehci_msc_read_blocks(uint8_t dev_addr, uint32_t lba, uint16_t block_count,
                          void* out_buffer, uint32_t out_capacity);
bool ehci_msc_write_blocks(uint8_t dev_addr, uint32_t lba, uint16_t block_count,
                           const void* in_buffer, uint32_t in_capacity);
/* Phase 20: request SCSI SYNCHRONIZE CACHE(10); false means the device did not confirm flush. */
bool ehci_msc_synchronize_cache(uint8_t dev_addr);

/* Re-scan root ports. Downstream hubs are not yet supported. */
void ehci_scan_ports();
/* Companion UHCI callback: enumerate/disconnect a full-/low-speed root port
   through the common device model and HID input path. */
void ehci_companion_enumerate(uint8_t port, bool low_speed);
bool ehci_companion_disconnect(uint8_t port, uint8_t address);

/* Non-IRQ service: handle latched root-port connect/disconnect events.
   Returns true when a root-port rescan was performed. */
bool ehci_service_port_changes();
/* Bounded polling for full-speed UHCI hub port status and child hotplug. */
bool ehci_service_hub_ports();
/* Check under interrupts-disabled idle handoff whether another scan is pending. */
bool ehci_port_change_pending();

} // namespace starlix::drivers::ehci
