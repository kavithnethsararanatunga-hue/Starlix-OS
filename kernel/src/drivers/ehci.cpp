/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#include "ehci.hpp"
#include "ehci_bar_validation.hpp"
#include "ehci_dma_guard.hpp"
#include "ehci_dma_lifetime.hpp"
#include "ehci_lifecycle_policy.hpp"
#include "ehci_periodic_budget.hpp"
#include "ehci_periodic_schedule.hpp"
#include "ehci_isochronous_policy.hpp"
#include "ehci_irq_event_latch.hpp"
#include "ehci_deferred_dispatch.hpp"
#include "ehci_request_policy.hpp"
#include "ehci_async_request_policy.hpp"
#include "ehci_usb_core_policy.hpp"
#include "ehci_root_port_policy.hpp"
#include "ehci_hub_policy.hpp"
#include "ehci_device_lifetime_policy.hpp"
#include "ehci_recovery_policy.hpp"
#include "ehci_power_policy.hpp"
#include "ehci_device_model_policy.hpp"
#include "ehci_storage_policy.hpp"
#include "ehci_hid_policy.hpp"
#include "ehci_hid_input_policy.hpp"
#include "ehci_cdc_acm_policy.hpp"
#include "ehci_usbnet_policy.hpp"
#include "ehci_audio_policy.hpp"
#include "ehci_video_policy.hpp"
#include "ehci_printer_policy.hpp"
#include "ehci_vendor_api_policy.hpp"
#include "ehci_class_registry_policy.hpp"
#include "ehci_security_policy.hpp"
#include "ehci_regression_policy.hpp"
#include "ehci_diagnostics_policy.hpp"
#include "../cpu/idt.hpp"
#include "pci.hpp"
#include "uhci.hpp"
#include "serial.hpp"
#include "../mem/phys.hpp"

namespace starlix::drivers::ehci {

/* EHCI-visible objects now come from the UEFI-map-backed DMA32 allocator.
   This still assumes Starlix identity-maps allocated RAM; no IOMMU or
   non-identity mapping backend exists yet. The configuration parser buffer
   is CPU-only and therefore remains ordinary static storage. */
static constexpr size_t EHCI_DMA_PAGE_SIZE = 4096u;
static constexpr size_t EHCI_MAX_TRANSFER_PAGES = 4u;
static constexpr size_t EHCI_MAX_TRANSFER_BYTES = EHCI_DMA_PAGE_SIZE * EHCI_MAX_TRANSFER_PAGES;
static uint32_t* periodic_frame_list = nullptr;
static EHCI_QTD* setup_qtd_storage = nullptr;
static EHCI_QTD* data_qtd_storage = nullptr;
static EHCI_QTD* status_qtd_storage = nullptr;
static uint8_t* setup_dma_page = nullptr;
static uint8_t* data_dma_pages[EHCI_MAX_TRANSFER_PAGES]{};
alignas(4096) static uint8_t configuration_buffer[4096];
static EHCI_QTD* bulk_qtd_storage = nullptr;
static uint8_t* bulk_dma_pages[EHCI_MAX_TRANSFER_PAGES]{};
static EHCI_QH* periodic_interrupt_qh = nullptr;
static EHCI_QTD* periodic_interrupt_qtd = nullptr;
static uint8_t* periodic_interrupt_dma_page = nullptr;
static EHCI_ITD* isochronous_itd_storage = nullptr;
static uint8_t* isochronous_dma_page = nullptr;
/* CPU-only fixed-capacity periodic bandwidth ledger. The persistent endpoint
   manager reserves here before inserting QHs into the live periodic schedule;
   the legacy one-shot API still uses its separate shared QH/qTD storage. */
static starlix::drivers::ehci::periodic::BandwidthLedger periodic_bandwidth_ledger{};
static uint32_t periodic_bandwidth_lock = 0u;
static uint32_t periodic_endpoint_lock = 0u;
static uint32_t periodic_endpoint_next_id = 1u;
static uint32_t periodic_endpoint_active = 0u;
static uint32_t ehci_deferred_error_events = 0u;
/* CPU-only buffer used by the multi-qTD short-packet regression test. */
alignas(16) static uint8_t ehci_request_smoke_data[EHCI_MAX_TRANSFER_BYTES]{};
static bool ehci_dma_storage_initialized = false;

struct USB_MSC_DEVICE {
    bool present;
    uint8_t device_address;
    uint8_t interface_number;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    uint32_t last_lba;
    uint32_t block_size;
    uint32_t next_tag;
    uint32_t bot_lock;       /* serializes CBW/data/CSW as one BOT transaction */
    uint32_t recovery_state; /* recovery_policy::DeviceState */
};

struct USBDeviceModelRecord {
    bool present;
    EHCIUSBDeviceInfo info;
};
static USBDeviceModelRecord usb_device_model[128]{};
static USB_MSC_DEVICE usb_msc_devices[128]{};

static constexpr uint32_t EHCI_HID_BINDING_CAPACITY = EHCI_PERIODIC_ENDPOINT_CAPACITY;
struct EHCIBoundHIDEndpoint {
    bool active;
    bool keyboard;
    bool generic_mouse;
    bool have_previous_report;
    uint8_t device_address;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t interval;
    uint16_t max_packet_size;
    uint32_t endpoint_id;
    uint32_t device_generation;
    uint8_t report[64];
    starlix::drivers::ehci::hid_policy::BootKeyboardReport previous_keyboard;
    starlix::drivers::ehci::hid_policy::BootMouseReport previous_mouse;
    starlix::drivers::ehci::hid_policy::GenericMouseDescriptor generic_mouse_layout;
};
static EHCIBoundHIDEndpoint ehci_hid_endpoints[EHCI_HID_BINDING_CAPACITY]{};
static starlix::drivers::ehci::hid_input::EventQueue ehci_hid_event_queue{};
static uint32_t ehci_hid_transfer_errors = 0u;
static_assert(static_cast<uint8_t>(EHCIUSBDriverBinding::Unbound) ==
              static_cast<uint8_t>(starlix::drivers::ehci::device_model_policy::Binding::Unbound));
static_assert(static_cast<uint8_t>(EHCIUSBDriverBinding::HubManagement) ==
              static_cast<uint8_t>(starlix::drivers::ehci::device_model_policy::Binding::HubManagement));
static_assert(static_cast<uint8_t>(EHCIUSBDriverBinding::MassStorageBOT) ==
              static_cast<uint8_t>(starlix::drivers::ehci::device_model_policy::Binding::MassStorageBOT));
static_assert(static_cast<uint8_t>(EHCIUSBDriverBinding::VendorGenericAPI) ==
              static_cast<uint8_t>(starlix::drivers::ehci::device_model_policy::Binding::VendorGenericAPI));
static_assert(static_cast<uint8_t>(EHCIUSBDriverBinding::HIDInput) ==
              static_cast<uint8_t>(starlix::drivers::ehci::device_model_policy::Binding::HIDInput));
static uint8_t bulk_data_toggle[128][16][2]{}; /* [address][endpoint][IN=1/OUT=0] */

static volatile EHCI_CAP_REGS* cap_regs = nullptr;
static volatile EHCI_OP_REGS* op_regs = nullptr;
static EHCI_QH* async_head_qh = nullptr;
static EHCI_QH* async_request_qh = nullptr; /* Single nonblocking request QH, linked only while active. */
static bool controller_active = false;
static bool controller_was_initialized = false;
/* If hardware cannot be proven halted, never reuse DMA memory. */
static bool controller_dma_quarantined = false;
static uint32_t num_ports = 0;
static uint32_t hcs_params = 0;
static uint8_t next_device_address = 1;
static uint16_t enumerated_port_mask = 0;
static uint8_t port_device_address[16] = {};
static bool usb_device_address_in_use[128] = {};
static uint8_t device_ep0_max_packet[128] = {};
static uint8_t usb_device_lifecycle_state[128] = {};
static bool usb_device_uses_uhci[128] = {};
static bool companion_enumeration_active = false;
static bool companion_enumeration_low_speed = false;
static bool hub_child_enumeration_active = false;
static uint8_t last_enumerated_address = 0u;
struct UHCIHubRecord {
    bool active;
    uint8_t address;
    uint8_t root_port;
    uint8_t port_count;
    uint8_t power_good_2ms;
    uint8_t child_address[8];
    uint64_t last_poll_ms;
};
static UHCIHubRecord uhci_hubs[4]{};
static bool uhci_hub_scan(UHCIHubRecord& hub, bool force);
static void uhci_hub_forget_root_port(uint8_t root_port);
static uint32_t usb_device_generation[128] = {};
/* The IRQ side publishes port-change generations only. Root-port scanning and
   enumeration must run in the non-IRQ service context. */
static uint32_t ehci_port_change_generation = 0;
static uint32_t ehci_port_change_serviced_generation = 0;

/* Phases 7-8: IRQ callbacks acknowledge causes and publish a compact event
   latch using lock-prefixed atomic operations. They never log, allocate memory,
   enumerate devices, block, or invoke scheduler code. */
static constexpr uint32_t EHCI_IRQ_CAUSE_MASK =
    STS_INT | STS_ERR | STS_PORT_CHANGE | STS_HOST_ERROR | STS_ASYNC_ADVANCE;
static constexpr uint32_t EHCI_IRQ_ENABLE_MASK = EHCI_IRQ_CAUSE_MASK;
/* IRQ-shared fields must use __atomic builtins for every concurrent access.
   32-bit atomics are lock-free on this x86_64 target and do not need libatomic. */
static starlix::drivers::ehci::irq::CauseLatch ehci_irq_event_latch{};
static uint8_t ehci_irq_line = 0xFFu;
static bool ehci_irq_enabled = false;

/* Phase 31 counters are CPU-only and do not allocate or log from IRQ context. */
static uint64_t ehci_diagnostic_values[starlix::drivers::ehci::diagnostics::Count]{};
static void ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::Counter id,
                                uint64_t amount = 1u) {
    if (static_cast<uint32_t>(id) >= starlix::drivers::ehci::diagnostics::Count) return;
    uint64_t* value = &ehci_diagnostic_values[static_cast<uint32_t>(id)];
    uint64_t old_value = __atomic_load_n(value, __ATOMIC_RELAXED);
    const uint64_t maximum = ~static_cast<uint64_t>(0u);
    for (;;) {
        const uint64_t next = amount > maximum - old_value ? maximum : old_value + amount;
        if (__atomic_compare_exchange_n(value, &old_value, next, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}
static void ehci_diagnostic_max(starlix::drivers::ehci::diagnostics::Counter id,
                                uint64_t observed) {
    if (static_cast<uint32_t>(id) >= starlix::drivers::ehci::diagnostics::Count) return;
    uint64_t* value = &ehci_diagnostic_values[static_cast<uint32_t>(id)];
    uint64_t old_value = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (observed > old_value &&
           !__atomic_compare_exchange_n(value, &old_value, observed, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}
struct EHCIIsoDiagnosticScope {
    bool succeeded;
    EHCIIsoDiagnosticScope() : succeeded(false) {
        ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::IsoAttempts);
    }
    ~EHCIIsoDiagnosticScope() {
        ehci_diagnostic_add(succeeded ? starlix::drivers::ehci::diagnostics::IsoSuccess
                                      : starlix::drivers::ehci::diagnostics::IsoErrors);
    }
};

/* Phase 9 request table. Slot descriptors remain immutable after Queued publish;
   a single DMA32 request QH and the fixed qTD/bounce arrays are held exclusively
   through completion/cancellation and the async-advance safe point. */
struct EHCIRequestSlot {
    uint32_t state;
    uint32_t request_id;
    EHCITransferRequest transfer;
    uint32_t result;
    uint32_t transferred;
    uint32_t cancel_requested;
};

enum class EHCIAsyncExecutionStage : uint32_t { Idle = 0u, InFlight = 1u, WaitingIaa = 2u };
struct EHCIAsyncExecution {
    uint32_t stage;
    uint32_t request_id;
    uint32_t qtd_count;
    uint32_t iaa_baseline;
    uint32_t terminal_result;
    uint64_t request_deadline_ms;
    uint64_t iaa_deadline_ms;
    uint64_t started_ms;
    uint16_t requested_bytes;
    uint8_t request_type;
    uint8_t data_in;
};
static EHCIAsyncExecution ehci_async_execution{};
static constexpr uint32_t EHCI_ASYNC_REQUEST_TIMEOUT_MS = 5000u;
static constexpr uint32_t EHCI_ASYNC_IAA_TIMEOUT_MS = 1000u;

static_assert(static_cast<uint32_t>(EHCIRequestState::Queued) ==
              starlix::drivers::ehci::request_policy::STATE_QUEUED);
static_assert(static_cast<uint32_t>(EHCIRequestState::Active) ==
              starlix::drivers::ehci::request_policy::STATE_ACTIVE);
static_assert(static_cast<uint32_t>(EHCIRequestState::Completed) ==
              starlix::drivers::ehci::request_policy::STATE_COMPLETED);
static_assert(static_cast<uint32_t>(EHCIRequestState::Failed) ==
              starlix::drivers::ehci::request_policy::STATE_FAILED);
static_assert(static_cast<uint32_t>(EHCIRequestState::Cancelled) ==
              starlix::drivers::ehci::request_policy::STATE_CANCELLED);
static_assert(static_cast<uint32_t>(EHCIRequestResult::Pending) ==
              starlix::drivers::ehci::request_policy::RESULT_PENDING);
static_assert(static_cast<uint32_t>(EHCIRequestResult::Success) ==
              starlix::drivers::ehci::request_policy::RESULT_SUCCESS);
static_assert(static_cast<uint32_t>(EHCIRequestResult::Cancelled) ==
              starlix::drivers::ehci::request_policy::RESULT_CANCELLED);

struct EHCIPersistentPeriodicEndpoint {
    bool active;
    uint32_t endpoint_id;
    uint32_t reservation_id;
    uint8_t device_address;
    uint8_t endpoint_address;
    uint8_t b_interval;
    uint8_t start_mask;
    uint16_t max_packet_size;
    uint16_t period_frames;
    EHCI_QH* qh;
    EHCI_QTD* qtd;
    uint8_t* dma_page;
    bool completion_ready;
    bool completion_error;
    uint16_t completed_bytes;
};
static EHCIPersistentPeriodicEndpoint periodic_endpoints[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
static EHCIIsochronousStats isochronous_stats{};
static EHCIRequestSlot ehci_request_slots[EHCI_REQUEST_CAPACITY]{};
static uint32_t ehci_request_queue_state = 0; /* 0=uninitialized, 1=init, 2=ready */
static uint32_t ehci_request_next_id = 1;
/* One non-IRQ dispatcher advances the single active hardware request state. */
static uint32_t ehci_request_service_busy = 0;
static bool ehci_request_lifecycle_self_test();
static bool ehci_request_transfer_smoke_test();
static uint32_t ehci_transfer_engine_busy = 0u;
static bool periodic_endpoint_schedule_rebuild();
static bool periodic_endpoint_capture_completion(EHCIPersistentPeriodicEndpoint& slot);
static bool periodic_endpoint_rearm(EHCIPersistentPeriodicEndpoint& slot);

/* Synchronous APIs and the async request state machine serialize ownership of
   the shared qTD/bounce storage. Async requests keep this guard across IAA. */
struct EHCITransferEngineGuard {
    bool acquired;
    EHCITransferEngineGuard() : acquired(false) {
        uint32_t expected = 0u;
        acquired = __atomic_compare_exchange_n(&ehci_transfer_engine_busy,
            &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    ~EHCITransferEngineGuard() {
        if (acquired) __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
    }
};

static void ehci_irq_handler(uint8_t irq) {
    (void)irq;
    ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::IRQCallbacks);
    volatile EHCI_OP_REGS* regs = op_regs;
    if (!regs) return;
    const uint32_t pending = starlix::drivers::ehci::irq::filter_causes(
        regs->usbsts, EHCI_IRQ_CAUSE_MASK);
    if (pending == 0u) return; /* Shared PCI INTx can be asserted by another device. */
    ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::EHCIIRQCauses);
    if (pending & STS_INT) ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::USBIntCauses);
    if (pending & STS_ERR) ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::USBErrCauses);
    if (pending & STS_PORT_CHANGE) ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::PortChangeCauses);
    if (pending & STS_HOST_ERROR) ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::HostErrorCauses);
    if (pending & STS_ASYNC_ADVANCE) ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::IAACauses);
    regs->usbsts = pending;    /* EHCI status causes are write-one-to-clear. */
    asm volatile("mfence" ::: "memory");
    /* Publish only acknowledged EHCI causes. Atomic OR coalesces cause bits
       without losing a concurrent producer; counters preserve callback counts. */
    (void)ehci_irq_event_latch.publish(pending, EHCI_IRQ_CAUSE_MASK,
                                       STS_ASYNC_ADVANCE);
    if (pending & STS_PORT_CHANGE)
        __atomic_fetch_add(&ehci_port_change_generation, 1u, __ATOMIC_RELEASE);
}

static void mem_zero(void* ptr, size_t size) {
    uint8_t* p = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) p[i] = 0;
}

static void copy_bytes(void* destination, const void* source, size_t size) {
    uint8_t* dst = static_cast<uint8_t*>(destination);
    const uint8_t* src = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < size; ++i) dst[i] = src[i];
}

static void zero_dma_pages(uint8_t* pages[EHCI_MAX_TRANSFER_PAGES]) {
    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i)
        if (pages[i]) mem_zero(pages[i], EHCI_DMA_PAGE_SIZE);
}

static void copy_to_dma_pages(uint8_t* pages[EHCI_MAX_TRANSFER_PAGES],
                              const void* source, size_t bytes) {
    const uint8_t* src = static_cast<const uint8_t*>(source);
    size_t offset = 0;
    while (offset < bytes) {
        const size_t page_index = offset / EHCI_DMA_PAGE_SIZE;
        const size_t page_offset = offset % EHCI_DMA_PAGE_SIZE;
        size_t chunk = EHCI_DMA_PAGE_SIZE - page_offset;
        if (chunk > bytes - offset) chunk = bytes - offset;
        copy_bytes(pages[page_index] + page_offset, src + offset, chunk);
        offset += chunk;
    }
}

static void copy_from_dma_pages(void* destination,
                                uint8_t* pages[EHCI_MAX_TRANSFER_PAGES],
                                size_t bytes) {
    uint8_t* dst = static_cast<uint8_t*>(destination);
    size_t offset = 0;
    while (offset < bytes) {
        const size_t page_index = offset / EHCI_DMA_PAGE_SIZE;
        const size_t page_offset = offset % EHCI_DMA_PAGE_SIZE;
        size_t chunk = EHCI_DMA_PAGE_SIZE - page_offset;
        if (chunk > bytes - offset) chunk = bytes - offset;
        copy_bytes(dst + offset, pages[page_index] + page_offset, chunk);
        offset += chunk;
    }
}

static void print_dec(uint32_t value) {
    char digits[11];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count) {
        char one[2] = { digits[--count], '\0' };
        starlix::drivers::serial::print(one);
    }
}

static void print_hex16(uint16_t value) {
    const char* hex = "0123456789ABCDEF";
    char text[5];
    text[0] = hex[(value >> 12) & 0xFu];
    text[1] = hex[(value >> 8) & 0xFu];
    text[2] = hex[(value >> 4) & 0xFu];
    text[3] = hex[value & 0xFu];
    text[4] = '\0';
    starlix::drivers::serial::print(text);
}

static inline void cpu_pause() {
    asm volatile("pause" ::: "memory");
}

static inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

static bool dma_range32(const void* pointer, size_t bytes,
                        size_t alignment, uint32_t& result) {
    /* Resolve only registered DMA allocations; never turn an arbitrary CPU
       pointer directly into an EHCI device address. The active backend is
       identity-mapped, but the API keeps CPU and device addresses distinct. */
    return starlix::mem::dma32_resolve(pointer, bytes, alignment, result);
}

static inline void dma_write_barrier() {
    starlix::drivers::ehci::dma::write_barrier();
}

static inline void dma_read_barrier() {
    starlix::drivers::ehci::dma::read_barrier();
}

static inline void dma_full_barrier() {
    starlix::drivers::ehci::dma::full_barrier();
}

static inline bool dma_sync_for_device(const void* pointer, size_t bytes) {
    return starlix::mem::dma32_sync_for_device(pointer, bytes);
}

static inline bool dma_sync_for_cpu(const void* pointer, size_t bytes) {
    return starlix::mem::dma32_sync_for_cpu(pointer, bytes);
}

static bool wait_controller_status(uint32_t mask, uint32_t expected, uint32_t iterations);
static bool stop_controller();

/* Phase 7A: lifecycle decisions must use observed controller status,
   not only the command bits written by software. */
static bool ehci_lifecycle_policy_self_test() {
    if (!starlix::drivers::ehci::lifecycle::self_test()) {
        starlix::drivers::serial::print(
            "[EHCI][LIFECYCLE] Controller status policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][LIFECYCLE] Controller lifecycle policy self-test passed (status, shutdown gates, drained owners, periodic ledger and reinit quarantine).\n");
    return true;
}

/* Phase 10: calculate a conservative high-speed interrupt schedule for the
   supported exponent interval range. USB 2.0 high-speed bInterval is expressed
   in microframes: 1, 2, 4, 8, ... 8192 microframes. For bInterval 1..4 the
   frame list links the QH in every frame and the QH S-mask selects which
   microframes can service it. Longer intervals select every 2^(bInterval-4)
   frames and microframe zero. This remains a one-QH-at-a-time scheduler. */
static bool periodic_schedule_parameters(uint8_t b_interval,
                                         uint16_t* period_frames,
                                         uint8_t* start_mask) {
    if (!period_frames || !start_mask) return false;
    starlix::drivers::ehci::periodic::IntervalParameters params{};
    if (!starlix::drivers::ehci::periodic::interval_parameters(b_interval, &params))
        return false;
    *period_frames = params.period_frames;
    *start_mask = params.start_mask;
    return true;
}

static bool build_periodic_frame_list(uint32_t qh_address, uint8_t b_interval,
                                      uint32_t* frame_list, uint16_t* period_frames) {
    uint8_t start_mask = 0u;
    if (!frame_list || !period_frames || (qh_address & 0x1Fu) != 0u ||
        qh_address == 0u ||
        !periodic_schedule_parameters(b_interval, period_frames, &start_mask)) return false;
    const uint16_t frames = *period_frames;
    for (uint32_t frame = 0; frame < 1024u; ++frame) {
        frame_list[frame] = (frame % frames == 0u)
            ? (qh_address | LINK_TYPE_QH) : LINK_TERMINATE;
    }
    return true;
}

static bool periodic_frame_list_layout_self_test() {
    if (!periodic_frame_list || !periodic_interrupt_qh) return false;
    uint32_t qh_address = 0u;
    if (!dma_range32(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh), 32u, qh_address))
        return false;
    uint16_t period = 0u;
    uint8_t start_mask = 0u;
    const uint32_t link = qh_address | LINK_TYPE_QH;
    const uint8_t intervals[] = {1u, 2u, 3u, 4u};
    const uint8_t masks[] = {0xFFu, 0x55u, 0x11u, 0x01u};
    for (size_t n = 0; n < sizeof(intervals) / sizeof(intervals[0]); ++n) {
        if (!periodic_schedule_parameters(intervals[n], &period, &start_mask) ||
            period != 1u || start_mask != masks[n] ||
            !build_periodic_frame_list(qh_address, intervals[n], periodic_frame_list, &period) ||
            period != 1u) return false;
        for (uint32_t i = 0; i < 1024u; ++i)
            if (periodic_frame_list[i] != link) return false;
    }
    if (!build_periodic_frame_list(qh_address, 5u, periodic_frame_list, &period) || period != 2u)
        return false;
    for (uint32_t i = 0; i < 1024u; ++i)
        if (periodic_frame_list[i] != ((i % 2u == 0u) ? link : LINK_TERMINATE)) return false;
    if (!build_periodic_frame_list(qh_address, 14u, periodic_frame_list, &period) || period != 1024u)
        return false;
    if (periodic_frame_list[0] != link) return false;
    for (uint32_t i = 1; i < 1024u; ++i) if (periodic_frame_list[i] != LINK_TERMINATE) return false;
    if (periodic_schedule_parameters(0u, &period, &start_mask) ||
        periodic_schedule_parameters(15u, &period, &start_mask) ||
        build_periodic_frame_list(qh_address, 0u, periodic_frame_list, &period) ||
        build_periodic_frame_list(qh_address, 15u, periodic_frame_list, &period)) return false;
    for (uint32_t i = 0; i < 1024u; ++i) periodic_frame_list[i] = LINK_TERMINATE;
    dma_write_barrier();
    starlix::drivers::serial::print("[EHCI][PERIODIC] 1024-frame interval layout self-test passed.\n");
    starlix::drivers::serial::print("[EHCI][PERIODIC] high-speed microframe S-mask self-test passed.\n");
    return true;
}

/* Validate admission, duplicate rejection, release accounting, interval cadence,
   and fixed-capacity failure. This tests only the CPU-side budget ledger. */
static bool periodic_schedule_policy_self_test() {
    using namespace starlix::drivers::ehci::periodic_schedule;
    const uint16_t periods[5] = {1u, 2u, 4u, 8u, 4u};
    const bool active[5] = {true, true, true, true, false};
    uint8_t order[5]{};
    uint32_t count = 0u;
    if (!build_order(periods, active, 5u, order, &count) || count != 4u) return false;
    if (periods[order[0]] != 8u || periods[order[1]] != 4u ||
        periods[order[2]] != 2u || periods[order[3]] != 1u) return false;
    if (frame_head(0u, periods, order, count) != order[0]) return false;
    if (periods[frame_head(4u, periods, order, count)] != 4u) return false;
    if (periods[frame_head(2u, periods, order, count)] != 2u) return false;
    if (periods[frame_head(1u, periods, order, count)] != 1u) return false;
    if (frame_head(3u, periods, order, count) != order[3]) return false;
    if (valid_period(3u) || valid_period(0u) || valid_period(2048u) || !valid_period(1024u))
        return false;
    starlix::drivers::serial::print("[EHCI][PERIODIC] Multi-QH frame-head scheduling self-test passed.\n");
    return true;
}

static bool periodic_bandwidth_self_test() {
    using starlix::drivers::ehci::periodic::BandwidthLedger;
    BandwidthLedger& ledger = periodic_bandwidth_ledger;
    ledger.reset();
    uint32_t a = 0u, b = 0u, c = 0u, d = 0u, duplicate = 0u;
    bool ok = ledger.reserve(1u, 0x81u, 512u, 3u, 1u, &a) &&
              ledger.reserve(1u, 0x82u, 512u, 3u, 1u, &b) &&
              ledger.reserve(1u, 0x83u, 512u, 3u, 1u, &c);
    uint16_t used = 0u;
    ok = ok && !ledger.reserve(1u, 0x84u, 512u, 3u, 1u, &d) && d == 0u;
    ok = ok && !ledger.reserve(1u, 0x81u, 8u, 1u, 4u, &duplicate) && duplicate == 0u;
    ok = ok && ledger.usage(0u, 0u, &used) && used == 5184u;
    ok = ok && ledger.usage(9u, 7u, &used) && used == 5184u;
    ok = ok && ledger.release(b) && ledger.usage(0u, 0u, &used) && used == 3456u;
    ok = ok && !ledger.release(b);
    ok = ok && ledger.reserve(2u, 0x81u, 512u, 3u, 1u, &d);
    ok = ok && ledger.usage(0u, 0u, &used) && used == 5184u;
    ok = ok && ledger.release(a) && ledger.release(c) && ledger.release(d);
    ok = ok && ledger.usage(0u, 0u, &used) && used == 0u;

    uint32_t e = 0u;
    ok = ok && ledger.reserve(3u, 0x81u, 64u, 1u, 2u, &e);
    ok = ok && ledger.usage(0u, 0u, &used) && used == 128u;
    ok = ok && ledger.usage(0u, 1u, &used) && used == 0u;
    ok = ok && ledger.usage(0u, 2u, &used) && used == 128u;
    ok = ok && ledger.release(e);

    uint32_t long_interval = 0u;
    ok = ok && ledger.reserve(4u, 0x81u, 64u, 1u, 14u, &long_interval);
    ok = ok && ledger.usage(0u, 0u, &used) && used == 128u;
    ok = ok && ledger.usage(1u, 0u, &used) && used == 0u;
    ok = ok && ledger.release(long_interval);
    ok = ok && !ledger.reserve(0u, 0x81u, 64u, 1u, 4u, &long_interval);
    ok = ok && !ledger.reserve(5u, 0x80u, 64u, 1u, 4u, &long_interval);
    ok = ok && !ledger.reserve(5u, 0x81u, 1025u, 1u, 4u, &long_interval);
    ok = ok && !ledger.reserve(5u, 0x81u, 64u, 4u, 4u, &long_interval);
    ok = ok && !ledger.reserve(5u, 0x81u, 64u, 1u, 0u, &long_interval);
    ok = ok && !ledger.usage(1024u, 0u, &used) && !ledger.usage(0u, 8u, &used);

    /* Sixteen small endpoints fit registry capacity; the seventeenth is rejected. */
    ledger.reset();
    uint32_t ids[16]{};
    for (uint32_t i = 0; i < 16u && ok; ++i)
        ok = ledger.reserve(static_cast<uint8_t>(i + 1u), 0x81u, 8u, 1u, 14u, &ids[i]);
    ok = ok && ledger.active_count() == 16u;
    ok = ok && !ledger.reserve(17u, 0x81u, 8u, 1u, 14u, &long_interval);
    for (uint32_t i = 0; i < 16u; ++i) ok = ledger.release(ids[i]) && ok;
    ledger.reset();
    if (!ok) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] bandwidth admission self-test failed.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][PERIODIC] bandwidth admission self-test passed.\n");
    return true;
}

/* Disable periodic traversal before touching any QH/qTD/buffer it could reach.
   If the schedule cannot be quiesced, stop the controller and quarantine DMA. */
static bool stop_periodic_schedule() {
    if (!op_regs) return false;
    op_regs->usbcmd &= ~CMD_PERIODIC_EN;
    dma_write_barrier();
    if (!wait_controller_status(STS_PERIODIC, 0u, 20000000u)) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Schedule did not stop; halting controller.\n");
        (void)stop_controller();
        return false;
    }
    dma_full_barrier();
    for (uint32_t i = 0; i < 1024u; ++i) periodic_frame_list[i] = LINK_TERMINATE;
    if (periodic_interrupt_qh) periodic_interrupt_qh->horizontal_link = LINK_TERMINATE;
    dma_write_barrier();
    return true;
}

/* Allocate independently owned, page-aligned DMA32 storage once. Each page
   is registered with the DMA mapper, and CPU pointers are used only for memory
   access; EHCI-visible addresses are obtained separately through dma_range32.
   Pages are not reclaimed yet because a failed controller halt must never let
   the device access reused memory. */
template <typename T>
static T* allocate_dma32_page() {
    starlix::mem::DmaRegion region{};
    if (!starlix::mem::dma32_alloc_page(region) || !region.valid ||
        !region.cpu_address || region.length != EHCI_DMA_PAGE_SIZE) return nullptr;
    return static_cast<T*>(region.cpu_address);
}

template <typename T>
static bool ensure_dma32_page(T*& storage) {
    return starlix::drivers::ehci::lifetime::ensure_once(
        storage, []() -> T* { return allocate_dma32_page<T>(); });
}

/* Phase 6 finalization: run every EHCI-owned DMA range through the explicit
   registry-backed synchronization API before handing any schedule to hardware.
   The supported backend is x86-64 coherent identity-mapped RAM; this routine
   fails closed if a page is absent or not registered. */
static bool dma_sync_api_self_test() {
    const bool registered_ranges_work =
        async_head_qh &&
        dma_sync_for_device(async_head_qh, sizeof(*async_head_qh)) &&
        dma_sync_for_cpu(async_head_qh, sizeof(*async_head_qh));
    /* configuration_buffer is ordinary CPU-only storage, never a DMA region. */
    const bool unregistered_ranges_rejected =
        !dma_sync_for_device(configuration_buffer, 16u) &&
        !dma_sync_for_cpu(configuration_buffer, 16u);
    if (!registered_ranges_work || !unregistered_ranges_rejected) {
        starlix::drivers::serial::print(
            "[EHCI][DMA] Registered-range synchronization self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][DMA] Sync API self-test passed (registered ranges accepted; CPU-only ranges rejected).\n");
    return true;
}

static bool sync_all_ehci_dma_for_device() {
    if (!periodic_frame_list || !async_head_qh || !async_request_qh || !setup_qtd_storage ||
        !data_qtd_storage || !status_qtd_storage || !setup_dma_page ||
        !bulk_qtd_storage || !periodic_interrupt_qh || !periodic_interrupt_qtd ||
        !periodic_interrupt_dma_page || !isochronous_itd_storage || !isochronous_dma_page) return false;

    if (!dma_sync_for_device(periodic_frame_list, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(async_head_qh, sizeof(*async_head_qh)) ||
        !dma_sync_for_device(async_request_qh, sizeof(*async_request_qh)) ||
        !dma_sync_for_device(setup_qtd_storage, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(data_qtd_storage, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(status_qtd_storage, sizeof(*status_qtd_storage)) ||
        !dma_sync_for_device(setup_dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(bulk_qtd_storage, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh)) ||
        !dma_sync_for_device(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd)) ||
        !dma_sync_for_device(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(isochronous_itd_storage, sizeof(*isochronous_itd_storage)) ||
        !dma_sync_for_device(isochronous_dma_page, EHCI_DMA_PAGE_SIZE)) return false;

    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
        if (!data_dma_pages[i] || !bulk_dma_pages[i] ||
            !dma_sync_for_device(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) ||
            !dma_sync_for_device(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE)) return false;
    }
    return true;
}

static bool ehci_dma_storage_init() {
    if (ehci_dma_storage_initialized) return true;
    if (!starlix::mem::phys_allocator_ready()) {
        starlix::drivers::serial::print("[EHCI][DMA] Physical DMA32 allocator is not ready.\n");
        return false;
    }

    /* DMA pages are boot-lifetime allocations because no safe unmap/free API
       exists yet. Keep successfully allocated pointers when a later allocation
       fails, so a retry never allocates a replacement and loses the old page. */
    bool pages_ok = true;
    pages_ok = ensure_dma32_page(periodic_frame_list) && pages_ok;
    pages_ok = ensure_dma32_page(async_head_qh) && pages_ok;
    pages_ok = ensure_dma32_page(async_request_qh) && pages_ok;
    pages_ok = ensure_dma32_page(setup_qtd_storage) && pages_ok;
    pages_ok = ensure_dma32_page(data_qtd_storage) && pages_ok;
    pages_ok = ensure_dma32_page(status_qtd_storage) && pages_ok;
    pages_ok = ensure_dma32_page(setup_dma_page) && pages_ok;
    pages_ok = ensure_dma32_page(bulk_qtd_storage) && pages_ok;
    pages_ok = ensure_dma32_page(periodic_interrupt_qh) && pages_ok;
    pages_ok = ensure_dma32_page(periodic_interrupt_qtd) && pages_ok;
    pages_ok = ensure_dma32_page(periodic_interrupt_dma_page) && pages_ok;
    pages_ok = ensure_dma32_page(isochronous_itd_storage) && pages_ok;
    pages_ok = ensure_dma32_page(isochronous_dma_page) && pages_ok;

    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
        pages_ok = ensure_dma32_page(data_dma_pages[i]) && pages_ok;
        pages_ok = ensure_dma32_page(bulk_dma_pages[i]) && pages_ok;
    }

    if (!periodic_frame_list || !async_head_qh || !async_request_qh || !setup_qtd_storage ||
        !data_qtd_storage || !status_qtd_storage || !setup_dma_page ||
        !bulk_qtd_storage || !periodic_interrupt_qh || !periodic_interrupt_qtd ||
        !periodic_interrupt_dma_page || !isochronous_itd_storage || !isochronous_dma_page || !pages_ok) {
        starlix::drivers::serial::print("[EHCI][DMA] DMA32 page allocation/registration failed; controller remains disabled.\n");
        return false;
    }

    ehci_dma_storage_initialized = true;
    starlix::drivers::serial::print("[OK] EHCI descriptors and four-page transfer buffers allocated through explicit DMA32 regions.\n");
    starlix::drivers::serial::print("[OK] EHCI periodic interrupt QH/qTD and buffer allocated through DMA32 regions.\n");
    starlix::drivers::serial::print("[OK] EHCI isochronous iTD and bounce page allocated through DMA32 regions.\n");
    return true;
}

/* Channel 2 of the legacy PIT is used only because Starlix does not yet have
   a kernel timer service. A bounded wait prevents a missing PIT from hanging
   the kernel forever. This should later be replaced by the kernel timer API. */
static bool delay_ms(uint32_t milliseconds) {
    const uint8_t original_port61 = starlix::cpu::inb(0x61);
    while (milliseconds) {
        const uint32_t chunk = milliseconds > 50u ? 50u : milliseconds;
        const uint32_t ticks32 = (1193182u * chunk) / 1000u;
        const uint16_t ticks = static_cast<uint16_t>(ticks32 ? ticks32 : 1u);

        starlix::cpu::outb(0x61, static_cast<uint8_t>(original_port61 & ~0x03u));
        starlix::cpu::outb(0x43, 0xB0); /* Counter 2, LSB/MSB, mode 0, binary. */
        starlix::cpu::outb(0x42, static_cast<uint8_t>(ticks & 0xFFu));
        starlix::cpu::outb(0x42, static_cast<uint8_t>((ticks >> 8) & 0xFFu));
        starlix::cpu::outb(0x61, static_cast<uint8_t>((original_port61 & ~0x02u) | 0x01u));

        uint32_t timeout = 20000000u;
        while ((starlix::cpu::inb(0x61) & 0x20u) == 0 && timeout) {
            cpu_pause();
            --timeout;
        }
        starlix::cpu::outb(0x61, original_port61);
        if (timeout == 0) return false;
        milliseconds -= chunk;
    }
    return true;
}

static bool wait_register(volatile uint32_t* reg, uint32_t mask,
                          uint32_t expected, uint32_t iterations) {
    while (iterations--) {
        if ((*reg & mask) == expected) return true;
        cpu_pause();
    }
    return false;
}

static bool wait_controller_status(uint32_t mask, uint32_t expected, uint32_t iterations) {
    if (!op_regs) return false;
    while (iterations--) {
        if ((op_regs->usbsts & mask) == expected) return true;
        cpu_pause();
    }
    return false;
}

static void clear_status(uint32_t flags) {
    if (op_regs) op_regs->usbsts = flags & STS_W1C_MASK;
}

/* PCI extended-capability linked list may contain capabilities before the
   USB legacy support block, so walk it instead of assuming the first entry. */
static bool release_legacy_ownership(uint8_t bus, uint8_t slot, uint8_t function,
                                     uint32_t hcc_params_value) {
    uint32_t offset = (hcc_params_value & HCC_EECP_MASK) >> HCC_EECP_SHIFT;
    if (offset < 0x40u || offset > 0xFCu) return true;

    for (uint32_t count = 0; count < 48u; ++count) {
        if (offset < 0x40u || offset > 0xFCu || (offset & 3u)) break;
        const uint32_t capability = starlix::drivers::pci::pci_read(
            bus, slot, function, static_cast<uint8_t>(offset));
        const uint8_t capability_id = static_cast<uint8_t>(capability & 0xFFu);
        const uint8_t next_offset = static_cast<uint8_t>((capability >> 8) & 0xFFu);

        if (capability_id == 0x01u) { /* USB Legacy Support */
            constexpr uint32_t BIOS_OWNED = 1u << 16;
            constexpr uint32_t OS_OWNED = 1u << 24;
            starlix::drivers::pci::pci_write(bus, slot, function,
                static_cast<uint8_t>(offset), capability | OS_OWNED);

            if (capability & BIOS_OWNED) {
                starlix::drivers::serial::print("[EHCI] Requested BIOS-to-OS ownership handoff.\n");
                for (uint32_t attempt = 0; attempt < 100u; ++attempt) {
                    const uint32_t current = starlix::drivers::pci::pci_read(
                        bus, slot, function, static_cast<uint8_t>(offset));
                    if ((current & BIOS_OWNED) == 0) {
                        starlix::drivers::serial::print("[EHCI] BIOS ownership released.\n");
                        return true;
                    }
                    if (!delay_ms(10)) break;
                }
                starlix::drivers::serial::print("[EHCI] ERROR: BIOS ownership handoff timed out.\n");
                return false;
            } else {
                starlix::drivers::serial::print("[EHCI] OS ownership bit set.\n");
            }
            return true;
        }
        if (next_offset == 0) break;
        offset = next_offset;
    }
    return true;
}

static bool stop_controller() {
    if (!op_regs) {
        controller_active = false;
        return false;
    }
    controller_active = false;
    op_regs->usbintr = 0;
    op_regs->usbcmd &= ~(CMD_RUN | CMD_ASYNC_EN | CMD_PERIODIC_EN | CMD_ASYNC_ADVANCE);
    compiler_barrier();
    const bool halted = wait_controller_status(STS_HALTED, STS_HALTED, 20000000u);
    const bool async_inactive = halted &&
        wait_controller_status(STS_ASYNC, 0u, 20000000u);
    const bool periodic_inactive = halted &&
        wait_controller_status(STS_PERIODIC, 0u, 20000000u);
    const bool dma_safe = starlix::drivers::ehci::lifetime::controller_dma_reuse_allowed(
        halted, async_inactive, periodic_inactive);
    if (dma_safe) {
        dma_full_barrier();
        starlix::drivers::serial::print(
            "[EHCI] Controller halted; async and periodic schedules inactive. DMA reuse remains guarded.\n");
    } else {
        controller_dma_quarantined = true;
        if (!halted) {
            starlix::drivers::serial::print(
                "[EHCI] FATAL: controller did not halt; DMA objects quarantined and EHCI reinit forbidden.\n");
        } else {
            starlix::drivers::serial::print(
                "[EHCI] FATAL: controller halt seen but a schedule stayed active; DMA objects quarantined and EHCI reinit forbidden.\n");
        }
    }
    return dma_safe;
}

/* Asynchronous Advance Doorbell establishes a safe point before the single
   static QH/qTD set is modified for the next synchronous request. */
static bool synchronize_async_schedule() {
    if (!controller_active || !op_regs) return false;
    const uint32_t iaa_before = ehci_irq_event_latch.auxiliary_count();
    clear_status(STS_ASYNC_ADVANCE);
    op_regs->usbcmd |= CMD_ASYNC_ADVANCE;
    dma_write_barrier();

    bool completed = false;
    uint32_t timeout = 10000000u;
    while (timeout--) {
        if (op_regs->usbsts & STS_ASYNC_ADVANCE) {
            completed = true;
            break;
        }
        /* The IRQ handler acknowledges USBSTS. Count IAA events separately so
           an interrupt cannot hide completion from the asynchronous doorbell waiter. */
        if (ehci_irq_enabled &&
            ehci_irq_event_latch.auxiliary_count() != iaa_before) {
            completed = true;
            break;
        }
        cpu_pause();
    }
    if (!completed) {
        starlix::drivers::serial::print("[EHCI] Async advance timed out; stopping controller.\n");
        stop_controller();
        return false;
    }
    if (op_regs->usbsts & STS_ASYNC_ADVANCE) clear_status(STS_ASYNC_ADVANCE);
    dma_full_barrier();
    return true;
}

static uint32_t make_qh_characteristics(uint8_t address, uint8_t max_packet) {
    return (static_cast<uint32_t>(address) & 0x7Fu) |
           QH_TOGGLE_CONTROL | QH_HEAD | QH_SPEED_HIGH |
           (static_cast<uint32_t>(max_packet) << QH_MAX_PACKET_SHIFT);
}

static uint32_t make_bulk_qh_characteristics(uint8_t address, uint8_t endpoint,
                                             uint16_t max_packet) {
    return (static_cast<uint32_t>(address) & 0x7Fu) |
           ((static_cast<uint32_t>(endpoint) & 0x0Fu) << 8) |
           QH_HEAD | QH_SPEED_HIGH |
           (static_cast<uint32_t>(max_packet & 0x07FFu) << QH_MAX_PACKET_SHIFT);
}

static void prepare_qtd(EHCI_QTD& qtd, uint32_t next, uint32_t alt_next,
                        uint32_t pid, uint16_t bytes, bool data_toggle,
                        uint32_t buffer_address) {
    mem_zero(&qtd, sizeof(qtd));
    qtd.next_qtd = next;
    qtd.alt_next_qtd = alt_next;
    qtd.token = QTD_ACTIVE | QTD_CERR_3 | (pid << 8) |
                (static_cast<uint32_t>(bytes) << QTD_BYTES_SHIFT) |
                (data_toggle ? QTD_TOGGLE : 0u);
    if (buffer_address) qtd.buffer[0] = buffer_address;
}

static bool qTD_failed(const EHCI_QTD& qtd) {
    const uint32_t token = qtd.token;
    return (token & QTD_ERROR_MASK) != 0;
}

static bool ehci_control_transfer_impl(uint8_t dev_addr, USB_SETUP_PACKET* setup,
                                   void* data_buf, uint16_t data_len,
                                   uint16_t* completed_bytes) {
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return false;
    if (completed_bytes) *completed_bytes = 0u;
    if (!controller_active || !async_head_qh || !setup || dev_addr > 127u) return false;
    if (data_len > EHCI_MAX_TRANSFER_BYTES) return false;
    if ((data_len != 0u) && !data_buf) return false;
    if (data_len != setup->wLength) return false;
    if (data_len != 0u && device_ep0_max_packet[dev_addr] == 0u) return false;

    const uint32_t data_qtd_count = data_len
        ? static_cast<uint32_t>((data_len + EHCI_DMA_PAGE_SIZE - 1u) / EHCI_DMA_PAGE_SIZE) : 0u;
    uint32_t setup_qtd_address = 0, data_qtd_address = 0, status_qtd_address = 0;
    uint32_t setup_buffer_address = 0, qh_address = 0;
    uint32_t data_buffer_addresses[EHCI_MAX_TRANSFER_PAGES]{};
    if (!dma_range32(setup_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, setup_qtd_address) ||
        !dma_range32(data_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, data_qtd_address) ||
        !dma_range32(status_qtd_storage, sizeof(*status_qtd_storage), 32u, status_qtd_address) ||
        !dma_range32(setup_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, setup_buffer_address) ||
        !dma_range32(async_head_qh, sizeof(*async_head_qh), 32u, qh_address)) {
        starlix::drivers::serial::print("[EHCI] ERROR: control DMA object invalid or above 4 GiB.\n");
        return false;
    }
    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
        if (!data_dma_pages[i] ||
            !dma_range32(data_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE,
                         data_buffer_addresses[i])) {
            starlix::drivers::serial::print("[EHCI] ERROR: control DMA page invalid or above 4 GiB.\n");
            return false;
        }
    }

    mem_zero(setup_dma_page, EHCI_DMA_PAGE_SIZE);
    zero_dma_pages(data_dma_pages);
    copy_bytes(setup_dma_page, setup, sizeof(USB_SETUP_PACKET));
    if (data_len && (setup->bmRequestType & 0x80u) == 0)
        copy_to_dma_pages(data_dma_pages, data_buf, data_len);

    const uint32_t first_after_setup = data_qtd_count ? data_qtd_address : status_qtd_address;
    prepare_qtd(*setup_qtd_storage, first_after_setup, LINK_TERMINATE,
                QTD_PID_SETUP, sizeof(USB_SETUP_PACKET), false, setup_buffer_address);
    const bool data_in = (setup->bmRequestType & 0x80u) != 0;
    const uint32_t data_pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
    for (uint32_t i = 0; i < data_qtd_count; ++i) {
        const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
        const uint16_t chunk = static_cast<uint16_t>(
            (data_len - offset) > EHCI_DMA_PAGE_SIZE ? EHCI_DMA_PAGE_SIZE : (data_len - offset));
        const uint32_t next = (i + 1u < data_qtd_count)
            ? data_qtd_address + ((i + 1u) * sizeof(EHCI_QTD)) : status_qtd_address;
        /* For control IN, a short packet skips remaining data qTDs directly to status. */
        prepare_qtd(data_qtd_storage[i], next,
                    data_in ? status_qtd_address : LINK_TERMINATE,
                    data_pid, chunk, true, data_buffer_addresses[i]);
    }

    const uint32_t status_pid = (data_len && data_in) ? QTD_PID_OUT : QTD_PID_IN;
    prepare_qtd(*status_qtd_storage, LINK_TERMINATE, LINK_TERMINATE,
                status_pid, 0, true, 0);
    status_qtd_storage->token |= QTD_IOC;

    async_head_qh->horizontal_link = qh_address | LINK_TYPE_QH;
    async_head_qh->ep_characteristics = make_qh_characteristics(
        dev_addr, device_ep0_max_packet[dev_addr] ? device_ep0_max_packet[dev_addr] : 64u);
    async_head_qh->ep_capabilities = (1u << 30);
    async_head_qh->current_qtd = 0;
    async_head_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.token = 0;
    for (unsigned i = 0; i < 5; ++i) async_head_qh->overlay.buffer[i] = 0;

    /* Synchronize descriptors and device-visible buffers before publishing the
       first qTD through the shared asynchronous head QH. The caller's buffer is
       CPU-only here; outbound bytes have already been copied into DMA pages. */
    bool dma_prepared =
        dma_sync_for_device(setup_dma_page, EHCI_DMA_PAGE_SIZE) &&
        dma_sync_for_device(setup_qtd_storage, sizeof(*setup_qtd_storage)) &&
        dma_sync_for_device(status_qtd_storage, sizeof(*status_qtd_storage)) &&
        dma_sync_for_device(async_head_qh, sizeof(*async_head_qh));
    if (data_qtd_count != 0u) {
        dma_prepared = dma_sync_for_device(data_qtd_storage,
            static_cast<size_t>(data_qtd_count) * sizeof(EHCI_QTD)) && dma_prepared;
        for (uint32_t i = 0; i < data_qtd_count; ++i)
            dma_prepared = dma_sync_for_device(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) && dma_prepared;
    }
    if (!dma_prepared) {
        starlix::drivers::serial::print("[EHCI][DMA] Control transfer sync rejected an unregistered DMA range.\n");
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();
    async_head_qh->overlay.next_qtd = setup_qtd_address;
    if (!dma_sync_for_device(async_head_qh, sizeof(*async_head_qh))) {
        starlix::drivers::serial::print("[EHCI][DMA] Control QH sync failed before schedule publication.\n");
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();

    bool completed = false;
    bool failed = false;
    bool short_in_packet = false;
    uint32_t timeout = 20000000u;
    while (timeout--) {
        dma_read_barrier();
        const bool setup_done = (setup_qtd_storage->token & QTD_ACTIVE) == 0;
        const bool status_done = (status_qtd_storage->token & QTD_ACTIVE) == 0;
        bool data_done = true;
        short_in_packet = false;
        for (uint32_t i = 0; i < data_qtd_count; ++i) {
            const uint32_t token = data_qtd_storage[i].token;
            if ((token & QTD_ACTIVE) == 0u) {
                if (qTD_failed(data_qtd_storage[i])) { failed = true; break; }
                const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
                const uint32_t chunk = (data_len - (i * EHCI_DMA_PAGE_SIZE)) > EHCI_DMA_PAGE_SIZE
                    ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
                    : data_len - (i * EHCI_DMA_PAGE_SIZE);
                if (remaining > chunk) { failed = true; break; }
                if (data_in && remaining != 0u) short_in_packet = true;
            } else if (!short_in_packet) {
                data_done = false;
            }
        }
        if ((setup_done && qTD_failed(*setup_qtd_storage)) ||
            (status_done && qTD_failed(*status_qtd_storage))) failed = true;
        if (failed) break;
        /* STATUS completion proves the controller reached the control status
           stage; for a short IN packet, later qTDs can remain ACTIVE by design. */
        if (setup_done && status_done &&
            (data_done || (data_in && short_in_packet))) {
            completed = true;
            break;
        }
        cpu_pause();
    }

    if (completed) {
        /* IAA is the asynchronous-schedule safe point: do not inspect or reuse
           controller-owned descriptor/buffer state until the QH advances past
           the submitted chain. The DMA pages themselves remain boot-lifetime. */
        if (!synchronize_async_schedule()) return false;
        bool dma_observed =
            dma_sync_for_cpu(setup_qtd_storage, sizeof(*setup_qtd_storage)) &&
            dma_sync_for_cpu(status_qtd_storage, sizeof(*status_qtd_storage)) &&
            dma_sync_for_cpu(async_head_qh, sizeof(*async_head_qh));
        if (data_qtd_count != 0u) {
            dma_observed = dma_sync_for_cpu(data_qtd_storage,
                static_cast<size_t>(data_qtd_count) * sizeof(EHCI_QTD)) && dma_observed;
            for (uint32_t i = 0; i < data_qtd_count; ++i)
                dma_observed = dma_sync_for_cpu(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) && dma_observed;
        }
        if (!dma_observed) {
            starlix::drivers::serial::print("[EHCI][DMA] Control completion sync failed; stopping controller.\n");
            (void)stop_controller();
            return false;
        }
        dma_read_barrier();
    }
    if (!completed || failed) {
        starlix::drivers::serial::print(completed
            ? "[EHCI] USB control transfer completed with a qTD error.\n"
            : "[EHCI] USB control transfer timed out.\n");
        stop_controller();
        return false;
    }

    uint32_t actual32 = 0u;
    for (uint32_t i = 0; i < data_qtd_count; ++i) {
        const uint32_t token = data_qtd_storage[i].token;
        if (token & QTD_ACTIVE) break; /* Short-IN alternate path skipped this and later qTDs. */
        const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
        const uint32_t chunk = (data_len - (i * EHCI_DMA_PAGE_SIZE)) > EHCI_DMA_PAGE_SIZE
            ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
            : data_len - (i * EHCI_DMA_PAGE_SIZE);
        if (remaining > chunk) return false;
        actual32 += chunk - remaining;
        if (data_in && remaining != 0u) break;
    }
    if (actual32 > data_len) return false;
    const uint16_t actual = static_cast<uint16_t>(actual32);
    if (data_len && data_in && actual) copy_from_dma_pages(data_buf, data_dma_pages, actual);
    if (completed_bytes) *completed_bytes = actual;
    return true;
}

/* Preserve the established synchronous public entry point. */
bool ehci_control_transfer(uint8_t dev_addr, USB_SETUP_PACKET* setup,
                           void* data_buf, uint16_t data_len) {
    using namespace starlix::drivers::ehci::diagnostics;
    ehci_diagnostic_add(ControlCalls);
    const uint64_t start_ms = starlix::cpu::timer_now_ms();
    uint16_t actual = 0u;
    if (starlix::drivers::uhci::uhci_owns_device(dev_addr) ||
        (dev_addr == 0u && companion_enumeration_active)) {
        const bool uhci_ok = starlix::drivers::uhci::uhci_control_transfer(
            dev_addr, setup, data_buf, data_len);
        ehci_diagnostic_add(uhci_ok ? ControlSuccess : ControlFailure);
        return uhci_ok;
    }
    const bool ok = ehci_control_transfer_impl(dev_addr, setup, data_buf, data_len, &actual);
    ehci_diagnostic_add(ok ? ControlSuccess : ControlFailure);
    if (ok && actual != 0u && setup)
        ehci_diagnostic_add((setup->bmRequestType & 0x80u) ? BytesIn : BytesOut, actual);
    if (starlix::cpu::timer_is_ready()) {
        const uint64_t end_ms = starlix::cpu::timer_now_ms();
        if (end_ms >= start_ms) ehci_diagnostic_max(MaxControlLatencyMs, end_ms - start_ms);
    }
    return ok;
}

/* Synchronous bulk transfer. This first implementation serializes one qTD at a
   time, caps the DMA bounce-buffer size at 4096 bytes, and polls with bounded
   waits. Endpoint toggles are kept independently by device/endpoint/direction. */
static bool ehci_bulk_transfer_impl(uint8_t dev_addr, uint8_t endpoint_address,
                                    uint16_t max_packet, void* data_buf, uint16_t data_len,
                                    uint16_t* transferred) {
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return false;
    if (transferred) *transferred = 0;
    if (!controller_active || !async_head_qh || dev_addr == 0 || dev_addr > 127u) return false;
    if ((endpoint_address & 0x70u) != 0u || (endpoint_address & 0x0Fu) == 0u) return false;
    if (max_packet == 0u || max_packet > 512u || data_len > EHCI_MAX_TRANSFER_BYTES) return false;
    if (data_len && !data_buf) return false;

    const uint8_t endpoint = endpoint_address & 0x0Fu;
    const uint8_t direction = (endpoint_address & 0x80u) ? 1u : 0u;
    const bool data_in = direction != 0;
    const uint32_t qtd_count = data_len
        ? static_cast<uint32_t>((data_len + EHCI_DMA_PAGE_SIZE - 1u) / EHCI_DMA_PAGE_SIZE) : 1u;
    uint32_t qtd_base = 0, qh_address = 0;
    uint32_t buffer_addresses[EHCI_MAX_TRANSFER_PAGES]{};
    if (!dma_range32(bulk_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, qtd_base) ||
        !dma_range32(async_head_qh, sizeof(*async_head_qh), 32u, qh_address)) {
        starlix::drivers::serial::print("[EHCI] ERROR: bulk descriptor or QH DMA range invalid.\n");
        return false;
    }
    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
        if (!bulk_dma_pages[i] ||
            !dma_range32(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE,
                         buffer_addresses[i])) {
            starlix::drivers::serial::print("[EHCI] ERROR: bulk DMA page invalid or above 4 GiB.\n");
            return false;
        }
    }

    zero_dma_pages(bulk_dma_pages);
    if (data_len && !data_in) copy_to_dma_pages(bulk_dma_pages, data_buf, data_len);
    const uint32_t pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
    for (uint32_t i = 0; i < qtd_count; ++i) {
        const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
        const uint16_t chunk = data_len
            ? static_cast<uint16_t>((data_len - offset) > EHCI_DMA_PAGE_SIZE
                ? EHCI_DMA_PAGE_SIZE : (data_len - offset)) : 0u;
        const uint32_t next = (i + 1u < qtd_count)
            ? qtd_base + ((i + 1u) * sizeof(EHCI_QTD)) : LINK_TERMINATE;
        prepare_qtd(bulk_qtd_storage[i], next, LINK_TERMINATE, pid,
                    chunk, false, chunk ? buffer_addresses[i] : 0u);
        /* IOC on each qTD allows completion to be noticed if an IN short packet
           terminates the chain before the final descriptor. */
        bulk_qtd_storage[i].token |= QTD_IOC;
    }

    async_head_qh->horizontal_link = qh_address | LINK_TYPE_QH;
    async_head_qh->ep_characteristics = make_bulk_qh_characteristics(dev_addr, endpoint, max_packet);
    async_head_qh->ep_capabilities = (1u << 30);
    async_head_qh->current_qtd = 0;
    async_head_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.token = bulk_data_toggle[dev_addr][endpoint][direction] ? QTD_TOGGLE : 0u;
    for (unsigned i = 0; i < 5; ++i) async_head_qh->overlay.buffer[i] = 0;
    bool dma_prepared =
        dma_sync_for_device(bulk_qtd_storage, static_cast<size_t>(qtd_count) * sizeof(EHCI_QTD)) &&
        dma_sync_for_device(async_head_qh, sizeof(*async_head_qh));
    for (uint32_t i = 0; i < qtd_count; ++i)
        dma_prepared = dma_sync_for_device(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE) && dma_prepared;
    if (!dma_prepared) {
        starlix::drivers::serial::print("[EHCI][DMA] Bulk transfer sync rejected an unregistered DMA range.\n");
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();
    async_head_qh->overlay.next_qtd = qtd_base;
    if (!dma_sync_for_device(async_head_qh, sizeof(*async_head_qh))) {
        starlix::drivers::serial::print("[EHCI][DMA] Bulk QH sync failed before schedule publication.\n");
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();

    bool completed = false;
    bool failed = false;
    uint32_t timeout = 20000000u;
    while (timeout--) {
        dma_read_barrier();
        bool all_done = true;
        bool short_in_packet = false;
        for (uint32_t i = 0; i < qtd_count; ++i) {
            const uint32_t token = bulk_qtd_storage[i].token;
            if (token & QTD_ACTIVE) {
                if (!short_in_packet) all_done = false;
                continue;
            }
            if (qTD_failed(bulk_qtd_storage[i])) { failed = true; break; }
            const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint32_t chunk = data_len
                ? ((data_len - offset) > EHCI_DMA_PAGE_SIZE
                    ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE) : data_len - offset) : 0u;
            if (remaining > chunk) { failed = true; break; }
            if (data_in && remaining != 0u) short_in_packet = true;
        }
        if (failed) break;
        if (all_done || short_in_packet) { completed = true; break; }
        cpu_pause();
    }
    if (!completed) {
        starlix::drivers::serial::print("[EHCI] Bulk transfer timed out; stopping controller before DMA reuse.\n");
        stop_controller();
        return false;
    }

    /* Wait for the EHCI async-advance safe point before CPU-side descriptor
       inspection or data copy-back. If it times out, stop_controller() handles
       quarantine and this path returns without reusing the DMA buffers. */
    if (!synchronize_async_schedule()) return false;
    bool dma_observed =
        dma_sync_for_cpu(bulk_qtd_storage, static_cast<size_t>(qtd_count) * sizeof(EHCI_QTD)) &&
        dma_sync_for_cpu(async_head_qh, sizeof(*async_head_qh));
    for (uint32_t i = 0; i < qtd_count; ++i)
        dma_observed = dma_sync_for_cpu(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE) && dma_observed;
    if (!dma_observed) {
        starlix::drivers::serial::print("[EHCI][DMA] Bulk completion sync failed; stopping controller.\n");
        (void)stop_controller();
        return false;
    }
    dma_read_barrier();
    /* Validate every completed qTD before reclaiming the shared schedule. */
    uint32_t actual32 = 0u;
    bool short_in_packet = false;
    for (uint32_t i = 0; i < qtd_count; ++i) {
        const uint32_t token = bulk_qtd_storage[i].token;
        if (token & QTD_ACTIVE) break;
        if (qTD_failed(bulk_qtd_storage[i])) failed = true;
        const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
        const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
        const uint32_t chunk = data_len
            ? ((data_len - offset) > EHCI_DMA_PAGE_SIZE
                ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE) : data_len - offset) : 0u;
        if (remaining > chunk) failed = true;
        else actual32 += chunk - remaining;
        if (data_in && remaining != 0u) { short_in_packet = true; break; }
    }
    if (failed || actual32 > data_len) {
        starlix::drivers::serial::print("[EHCI] Bulk transfer qTD chain reported an error.\n");
        return false;
    }
    const uint16_t actual = static_cast<uint16_t>(actual32);
    if (data_in && actual) copy_from_dma_pages(data_buf, bulk_dma_pages, actual);
    bulk_data_toggle[dev_addr][endpoint][direction] =
        (async_head_qh->overlay.token & QTD_TOGGLE) ? 1u : 0u;
    if (transferred) *transferred = actual;
    (void)short_in_packet;
    return true;
}

/* Public bulk wrapper records bounded transfer outcomes and observed duration.
   Counters are diagnostic only; they do not change scheduling decisions. */
bool ehci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint_address,
                        uint16_t max_packet, void* data_buf, uint16_t data_len,
                        uint16_t* transferred) {
    using namespace starlix::drivers::ehci::diagnostics;
    ehci_diagnostic_add(BulkCalls);
    const uint64_t start_ms = starlix::cpu::timer_now_ms();
    uint16_t actual = 0u;
    const bool ok = ehci_bulk_transfer_impl(dev_addr, endpoint_address, max_packet,
                                            data_buf, data_len, &actual);
    if (transferred) *transferred = actual;
    ehci_diagnostic_add(ok ? BulkSuccess : BulkFailure);
    if (ok && actual != 0u) {
        ehci_diagnostic_add((endpoint_address & 0x80u) ? BytesIn : BytesOut, actual);
    }
    if (starlix::cpu::timer_is_ready()) {
        const uint64_t end_ms = starlix::cpu::timer_now_ms();
        if (end_ms >= start_ms) ehci_diagnostic_max(MaxBulkLatencyMs, end_ms - start_ms);
    }
    return ok;
}

/* One-shot high-speed interrupt endpoint transfer. This first periodic-schedule
   slice intentionally supports bInterval 4..14 (at least one frame between
   polls), one transaction <= max_packet, and one QH at a time. It is synchronous:
   it installs a periodic QH, waits with bounded polling, disables the periodic
   schedule, and only then returns/copies data. It is not a persistent poller. */
bool ehci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint_address,
                             uint16_t max_packet, uint8_t b_interval,
                             void* data_buf, uint16_t data_len,
                             uint16_t* transferred) {
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return false;
    if (transferred) *transferred = 0u;
    if (!controller_active || !op_regs || !periodic_frame_list ||
        !periodic_interrupt_qh || !periodic_interrupt_qtd || !periodic_interrupt_dma_page ||
        dev_addr == 0u || dev_addr > 127u) return false;
    if ((endpoint_address & 0x70u) != 0u || (endpoint_address & 0x0Fu) == 0u ||
        max_packet == 0u || max_packet > 1024u || data_len == 0u ||
        data_len > max_packet || data_len > EHCI_DMA_PAGE_SIZE || !data_buf ||
        b_interval == 0u || b_interval > 14u) return false;
    if (op_regs->usbcmd & CMD_PERIODIC_EN) return false;

    const uint8_t endpoint = endpoint_address & 0x0Fu;
    const uint8_t direction = (endpoint_address & 0x80u) ? 1u : 0u;
    const bool data_in = direction != 0u;
    uint32_t qh_address = 0u, qtd_address = 0u, buffer_address = 0u;
    if (!dma_range32(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh), 32u, qh_address) ||
        !dma_range32(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd), 32u, qtd_address) ||
        !dma_range32(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, buffer_address)) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Interrupt DMA object failed range/alignment checks.\n");
        return false;
    }
    uint16_t period_frames = 0u;
    uint8_t start_mask = 0u;
    if (!periodic_schedule_parameters(b_interval, &period_frames, &start_mask) ||
        !build_periodic_frame_list(qh_address, b_interval, periodic_frame_list, &period_frames))
        return false;
    mem_zero(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE);
    if (!data_in) copy_bytes(periodic_interrupt_dma_page, data_buf, data_len);

    periodic_interrupt_qtd->next_qtd = LINK_TERMINATE;
    periodic_interrupt_qtd->alt_next_qtd = LINK_TERMINATE;
    periodic_interrupt_qtd->token = QTD_ACTIVE | QTD_CERR_3 |
        (static_cast<uint32_t>(data_in ? QTD_PID_IN : QTD_PID_OUT) << 8) |
        (static_cast<uint32_t>(data_len) << QTD_BYTES_SHIFT) | QTD_IOC |
        (bulk_data_toggle[dev_addr][endpoint][direction] ? QTD_TOGGLE : 0u);
    periodic_interrupt_qtd->buffer[0] = buffer_address;
    for (unsigned i = 1; i < 5; ++i) periodic_interrupt_qtd->buffer[i] = 0u;

    periodic_interrupt_qh->horizontal_link = LINK_TERMINATE;
    periodic_interrupt_qh->ep_characteristics =
        (static_cast<uint32_t>(dev_addr) & 0x7Fu) |
        (static_cast<uint32_t>(endpoint) << 8) | QH_TOGGLE_CONTROL | QH_SPEED_HIGH |
        (static_cast<uint32_t>(max_packet) << QH_MAX_PACKET_SHIFT);
    /* S-mask selects legal high-speed microframes for this bInterval.
       No split completion mask; Mult=1 transaction. */
    periodic_interrupt_qh->ep_capabilities = static_cast<uint32_t>(start_mask) | (1u << 30);
    periodic_interrupt_qh->current_qtd = 0u;
    periodic_interrupt_qh->overlay.next_qtd = qtd_address;
    periodic_interrupt_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    periodic_interrupt_qh->overlay.token = 0u;
    for (unsigned i = 0; i < 5; ++i) periodic_interrupt_qh->overlay.buffer[i] = 0u;
    if (!dma_sync_for_device(periodic_frame_list, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh)) ||
        !dma_sync_for_device(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd)) ||
        !dma_sync_for_device(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE)) {
        starlix::drivers::serial::print("[EHCI][DMA] Periodic transfer sync rejected an unregistered DMA range.\n");
        return false;
    }
    dma_write_barrier();

    op_regs->usbcmd |= CMD_PERIODIC_EN;
    dma_write_barrier();
    if (!wait_controller_status(STS_PERIODIC, STS_PERIODIC, 20000000u)) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Periodic schedule failed to start.\n");
        (void)stop_periodic_schedule();
        return false;
    }

    bool completed = false;
    uint16_t last_frame = static_cast<uint16_t>((op_regs->frindex >> 3) & 0x03FFu);
    uint32_t elapsed_frames = 0u;
    const uint32_t timeout_ms = static_cast<uint32_t>(period_frames) + 8u;
    for (uint32_t elapsed_ms = 0u; elapsed_ms <= timeout_ms; ++elapsed_ms) {
        dma_read_barrier();
        const uint32_t token = periodic_interrupt_qtd->token;
        if ((token & QTD_ACTIVE) == 0u) { completed = true; break; }
        const uint16_t current_frame = static_cast<uint16_t>((op_regs->frindex >> 3) & 0x03FFu);
        const uint16_t frame_delta = static_cast<uint16_t>((current_frame - last_frame) & 0x03FFu);
        elapsed_frames += frame_delta;
        last_frame = current_frame;
        if (elapsed_frames >= static_cast<uint32_t>(period_frames) + 8u) break;
        /* No kernel timer service exists yet. Use the bounded legacy PIT delay
           so long bInterval values cannot spin out before the requested frame. */
        if (!delay_ms(1u)) break;
    }

    if (!stop_periodic_schedule()) return false;
    if (!dma_sync_for_cpu(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd)) ||
        !dma_sync_for_cpu(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh)) ||
        !dma_sync_for_cpu(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_cpu(periodic_frame_list, EHCI_DMA_PAGE_SIZE)) {
        starlix::drivers::serial::print("[EHCI][DMA] Periodic completion sync failed after schedule stop.\n");
        (void)stop_controller();
        return false;
    }
    dma_read_barrier();
    const uint32_t final_token = periodic_interrupt_qtd->token;
    if (!completed || (final_token & QTD_ACTIVE) != 0u || qTD_failed(*periodic_interrupt_qtd)) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Interrupt endpoint transfer failed or timed out.\n");
        return false;
    }
    const uint32_t remaining = (final_token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
    if (remaining > data_len) return false;
    const uint16_t actual = static_cast<uint16_t>(data_len - remaining);
    if (data_in && actual) copy_bytes(data_buf, periodic_interrupt_dma_page, actual);
    bulk_data_toggle[dev_addr][endpoint][direction] = (final_token & QTD_TOGGLE) ? 1u : 0u;
    if (transferred) *transferred = actual;
    return true;
}

static void write_port_control(uint32_t port, uint32_t control_bits, uint32_t changes_to_clear = 0) {
    if (!op_regs || port >= num_ports) return;
    op_regs->portsc[port] = (control_bits & PORT_SAFE_CONTROL_MASK) |
                            (changes_to_clear & PORT_CHANGE_MASK);
}

static void acknowledge_port_changes(uint32_t port, uint32_t status) {
    const uint32_t preserve = status & (PORT_POWER | PORT_OWNER | PORT_ENABLE | PORT_SUSPEND);
    const uint32_t changes = status & PORT_CHANGE_MASK;
    if (changes) write_port_control(port, preserve, changes);
}

static bool reset_root_port(uint32_t port) {
    uint32_t status = op_regs->portsc[port];
    if ((status & PORT_CONNECT) == 0) return false;
    if (status & PORT_OWNER) {
        starlix::drivers::serial::print("[EHCI] Port is already owned by a companion controller.\n");
        return false;
    }

    acknowledge_port_changes(port, status);
    status = op_regs->portsc[port];

    /* EHCI must hand low-speed K-state devices to a companion BEFORE reset. */
    if ((status & PORT_ENABLE) == 0 &&
        ((status & PORT_LINE_STATUS_MASK) >> 10) == 1u) {
        const uint32_t companions = (hcs_params & HCS_N_CC_MASK) >> HCS_N_CC_SHIFT;
        if (companions != 0u) {
            write_port_control(port, (status & PORT_POWER) | PORT_OWNER,
                               status & PORT_CHANGE_MASK);
            starlix::drivers::serial::print("[EHCI] Low-speed K-state detected; handed port to companion before reset.\n");
        } else {
            starlix::drivers::serial::print("[EHCI] Low-speed K-state device found but no companion controller exists.\n");
        }
        return false;
    }

    uint32_t base_control = status & PORT_POWER;

    /* EHCI 1.0 requires at least 50 ms reset assertion, then up to 2 ms to settle. */
    write_port_control(port, base_control | PORT_RESET);
    if (!delay_ms(50)) {
        write_port_control(port, base_control);
        starlix::drivers::serial::print("[EHCI] PIT delay unavailable; skipped unsafe port reset.\n");
        return false;
    }
    write_port_control(port, base_control);
    if (!delay_ms(2)) {
        starlix::drivers::serial::print("[EHCI] PIT delay unavailable after reset.\n");
        return false;
    }

    status = op_regs->portsc[port];
    acknowledge_port_changes(port, status);
    status = op_regs->portsc[port];
    if ((status & PORT_CONNECT) == 0) {
        starlix::drivers::serial::print("[EHCI] Device disconnected during port reset.\n");
        return false;
    }
    if ((status & PORT_ENABLE) == 0) {
        const uint32_t companions = (hcs_params & HCS_N_CC_MASK) >> HCS_N_CC_SHIFT;
        if (companions != 0) {
            write_port_control(port, (status & (PORT_POWER | PORT_ENABLE | PORT_SUSPEND)) | PORT_OWNER,
                               status & PORT_CHANGE_MASK);
            starlix::drivers::serial::print("[EHCI] Full/low-speed port handed to USB companion controller.\n");
        } else {
            starlix::drivers::serial::print("[EHCI] Connected device did not enable as high-speed USB.\n");
        }
        return false;
    }
    starlix::drivers::serial::print("[EHCI] High-speed root port reset and enabled.\n");
    return true;
}

static bool validate_configuration_descriptor(const uint8_t* bytes, uint16_t total_length) {
    starlix::drivers::ehci::usb_core::ConfigurationSummary summary{};
    return starlix::drivers::ehci::usb_core::parse_configuration(bytes, total_length, &summary);
}

#pragma pack(push, 1)
struct USB_MSC_CBW {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[16];
};
struct USB_MSC_CSW {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
};
#pragma pack(pop)
static_assert(sizeof(USB_MSC_CBW) == 31, "USB MSC CBW must be 31 bytes");
static_assert(sizeof(USB_MSC_CSW) == 13, "USB MSC CSW must be 13 bytes");

struct MSCBOTCommandGuard {
    USB_MSC_DEVICE* device;
    bool acquired;
    explicit MSCBOTCommandGuard(USB_MSC_DEVICE& d) : device(&d), acquired(false) {
        uint32_t expected = 0u;
        acquired = __atomic_compare_exchange_n(&device->bot_lock, &expected, 1u, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    ~MSCBOTCommandGuard() {
        if (acquired) __atomic_store_n(&device->bot_lock, 0u, __ATOMIC_RELEASE);
    }
};

static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}
static void write_be32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}
static void write_be16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

static bool msc_clear_halt(USB_MSC_DEVICE& device, uint8_t endpoint_address) {
    USB_SETUP_PACKET clear_halt{0x02u, USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
                                endpoint_address, 0u};
    if (!ehci_control_transfer(device.device_address, &clear_halt, nullptr, 0)) return false;
    const uint8_t ep = endpoint_address & 0x0Fu;
    const uint8_t dir = (endpoint_address & 0x80u) ? 1u : 0u;
    bulk_data_toggle[device.device_address][ep][dir] = 0;
    return true;
}

static bool msc_bot_reset_recovery(USB_MSC_DEVICE& device) {
    __atomic_store_n(&device.recovery_state,
        static_cast<uint32_t>(starlix::drivers::ehci::recovery_policy::DeviceState::Recovering),
        __ATOMIC_RELEASE);
    USB_SETUP_PACKET reset{0x21u, 0xFFu, 0u, device.interface_number, 0u};
    const bool reset_ok = ehci_control_transfer(device.device_address, &reset, nullptr, 0);
    /* Attempt both endpoint clear-halts even if reset failed; a recovery is
       considered successful only when all three required steps succeeded. */
    const bool in_ok = msc_clear_halt(device, device.bulk_in_endpoint);
    const bool out_ok = msc_clear_halt(device, device.bulk_out_endpoint);
    const auto final_state = starlix::drivers::ehci::recovery_policy::after_recovery(
        reset_ok, in_ok, out_ok);
    __atomic_store_n(&device.recovery_state, static_cast<uint32_t>(final_state), __ATOMIC_RELEASE);
    starlix::drivers::serial::print(final_state == starlix::drivers::ehci::recovery_policy::DeviceState::Ready
        ? "[EHCI][MSC] BOT reset recovery completed; device ready.\n"
        : "[EHCI][MSC] BOT reset recovery failed; device marked offline.\n");
    return final_state == starlix::drivers::ehci::recovery_policy::DeviceState::Ready;
}

static bool msc_bot_command(USB_MSC_DEVICE& device, const uint8_t* cdb,
                            uint8_t cdb_length, bool data_in, void* data,
                            uint32_t data_length) {
    using starlix::drivers::ehci::recovery_policy::DeviceState;
    using starlix::drivers::ehci::recovery_policy::BotOutcome;
    if (!device.present || !cdb || cdb_length == 0u || cdb_length > 16u ||
        data_length > EHCI_MAX_TRANSFER_BYTES || (data_length && !data)) return false;

    MSCBOTCommandGuard command_guard(device);
    const DeviceState state = static_cast<DeviceState>(
        __atomic_load_n(&device.recovery_state, __ATOMIC_ACQUIRE));
    if (!starlix::drivers::ehci::recovery_policy::command_admitted(
            device.present, state, command_guard.acquired) || !command_guard.acquired) {
        starlix::drivers::serial::print("[EHCI][MSC] BOT command rejected: busy or device recovery/offline state.\n");
        return false;
    }

    USB_MSC_CBW cbw{};
    cbw.signature = 0x43425355u; /* USB mass-storage CBW signature: bytes 'USBC' on x86. */
    cbw.tag = device.next_tag + 1u;
    if (cbw.tag == 0u) cbw.tag = 1u;
    device.next_tag = cbw.tag;
    cbw.transfer_length = data_length;
    cbw.flags = (data_length && data_in) ? 0x80u : 0u;
    cbw.lun = 0u;
    cbw.cdb_length = cdb_length;
    copy_bytes(cbw.cdb, cdb, cdb_length);
    if (!starlix::drivers::ehci::security_policy::bot_cbw_valid(
            cbw.signature, cbw.tag, cbw.transfer_length, cbw.flags, cbw.lun, cbw.cdb_length)) {
        starlix::drivers::serial::print("[EHCI][SECURITY] Refused malformed BOT CBW before bus submission.\n");
        return false;
    }

    uint16_t actual = 0u;
    bool transport_ok = ehci_bulk_transfer(device.device_address, device.bulk_out_endpoint,
        device.bulk_out_max_packet, &cbw, sizeof(cbw), &actual) && actual == sizeof(cbw);
    uint32_t data_actual = 0u;
    if (transport_ok && data_length != 0u) {
        actual = 0u;
        const bool data_transfer_ok = ehci_bulk_transfer(device.device_address,
            data_in ? device.bulk_in_endpoint : device.bulk_out_endpoint,
            data_in ? device.bulk_in_max_packet : device.bulk_out_max_packet,
            data, static_cast<uint16_t>(data_length), &actual);
        transport_ok = data_transfer_ok && actual <= data_length &&
                       (data_in || actual == data_length);
        if (transport_ok) data_actual = actual;
    }

    USB_MSC_CSW csw{};
    bool csw_read = false;
    if (transport_ok) {
        actual = 0u;
        csw_read = ehci_bulk_transfer(device.device_address, device.bulk_in_endpoint,
            device.bulk_in_max_packet, &csw, sizeof(csw), &actual) && actual == sizeof(csw);
        transport_ok = csw_read;
    }

    const bool csw_valid = csw_read &&
        starlix::drivers::ehci::recovery_policy::csw_structurally_valid(
            csw.signature, 0x53425355u, csw.tag, cbw.tag,
            csw.residue, data_length, csw.status) &&
        starlix::drivers::ehci::security_policy::bot_csw_valid(
            csw.signature, cbw.tag, csw.tag, csw.residue, data_length, csw.status);
    const bool data_length_matches = csw_valid && csw.residue <= data_length &&
        (data_length == 0u ? data_actual == 0u :
         (data_in ? (data_actual + csw.residue == data_length)
                  : (data_actual == data_length && csw.residue == 0u)));
    const BotOutcome outcome = starlix::drivers::ehci::recovery_policy::classify_bot_outcome(
        transport_ok, csw_valid, csw.status, csw.residue, data_length, data_length_matches);

    if (outcome == BotOutcome::Recover) {
        starlix::drivers::serial::print("[EHCI][MSC] BOT transport/CSW phase error; attempting serialized reset recovery.\n");
        if (controller_active) (void)msc_bot_reset_recovery(device);
        else __atomic_store_n(&device.recovery_state, static_cast<uint32_t>(DeviceState::Offline), __ATOMIC_RELEASE);
        return false;
    }
    if (outcome == BotOutcome::CommandFailed) {
        /* A structurally valid CSW with status=1 is an SCSI command failure,
           not a transport phase error; do not unnecessarily reset the device. */
        starlix::drivers::serial::print("[EHCI][MSC] SCSI command failed or completed with residue.\n");
        return false;
    }
    return true;
}

static bool bind_mass_storage(uint8_t dev_addr, const uint8_t* descriptors,
                              uint16_t total_length) {
    if (!descriptors || total_length < 9u || dev_addr == 0u || dev_addr > 127u) return false;
    bool in_msc_interface = false;
    uint8_t interface_number = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t max_in = 0, max_out = 0;

    uint16_t offset = 0;
    while (offset + 2u <= total_length) {
        const uint8_t length = descriptors[offset];
        const uint8_t type = descriptors[offset + 1u];
        if (length < 2u || offset + length > total_length) return false;
        if (type == USB_DESC_INTERFACE) {
            in_msc_interface = false;
            if (length >= 9u && descriptors[offset + 3u] == 0u &&
                descriptors[offset + 5u] == USB_CLASS_MASS_STORAGE &&
                descriptors[offset + 6u] == USB_MSC_SUBCLASS_SCSI &&
                descriptors[offset + 7u] == USB_MSC_PROTOCOL_BOT) {
                in_msc_interface = true;
                interface_number = descriptors[offset + 2u];
            }
        } else if (type == USB_DESC_ENDPOINT && in_msc_interface && length >= 7u) {
            const uint8_t address = descriptors[offset + 2u];
            const uint8_t attributes = descriptors[offset + 3u];
            const uint16_t packet = static_cast<uint16_t>(descriptors[offset + 4u] |
                (static_cast<uint16_t>(descriptors[offset + 5u]) << 8)) & 0x07FFu;
            /* This enumeration path only configures EHCI high-speed devices;
               high-speed bulk endpoints use 512-byte maximum packets. */
            if ((attributes & 0x03u) == 0x02u &&
                starlix::drivers::ehci::recovery_policy::valid_high_speed_bulk_packet(packet)) {
                if (address & 0x80u) { ep_in = address; max_in = packet; }
                else { ep_out = address; max_out = packet; }
            }
        }
        offset = static_cast<uint16_t>(offset + length);
    }
    if (ep_in == 0u || ep_out == 0u || max_in == 0u || max_out == 0u) return false;

    USB_MSC_DEVICE& device = usb_msc_devices[dev_addr];
    device = USB_MSC_DEVICE{};
    device.device_address = dev_addr;
    device.interface_number = interface_number;
    device.bulk_in_endpoint = ep_in;
    device.bulk_out_endpoint = ep_out;
    device.bulk_in_max_packet = max_in;
    device.bulk_out_max_packet = max_out;
    device.next_tag = 0u;
    device.bot_lock = 0u;
    device.recovery_state = static_cast<uint32_t>(
        starlix::drivers::ehci::recovery_policy::DeviceState::Ready);
    bulk_data_toggle[dev_addr][ep_in & 0x0Fu][1] = 0;
    bulk_data_toggle[dev_addr][ep_out & 0x0Fu][0] = 0;
    /* Mark present before BOT command dispatch; capacity becomes valid afterward. */
    device.present = true;
    starlix::drivers::serial::print("[EHCI][MSC] USB Mass Storage BOT interface found. IN endpoint 0x");
    print_hex16(ep_in);
    starlix::drivers::serial::print(", OUT endpoint 0x");
    print_hex16(ep_out);
    starlix::drivers::serial::print(".\n");

    uint8_t inquiry_cdb[6]{};
    inquiry_cdb[0] = 0x12u; inquiry_cdb[4] = 36u;
    uint8_t inquiry_data[36]{};
    if (!msc_bot_command(device, inquiry_cdb, sizeof(inquiry_cdb), true,
                         inquiry_data, sizeof(inquiry_data))) {
        device.present = false;
        return false;
    }
    char vendor[9]{}; char product[17]{};
    copy_bytes(vendor, inquiry_data + 8, 8);
    copy_bytes(product, inquiry_data + 16, 16);
    starlix::drivers::serial::print("[EHCI][MSC] SCSI INQUIRY: ");
    starlix::drivers::serial::print(vendor); starlix::drivers::serial::print(" ");
    starlix::drivers::serial::print(product); starlix::drivers::serial::print("\n");

    uint8_t capacity_cdb[10]{};
    capacity_cdb[0] = 0x25u; /* READ CAPACITY(10) */
    uint8_t capacity_data[8]{};
    if (!msc_bot_command(device, capacity_cdb, sizeof(capacity_cdb), true,
                         capacity_data, sizeof(capacity_data))) {
        device.present = false;
        starlix::drivers::serial::print("[EHCI][MSC] READ CAPACITY failed; block device unavailable.\n");
        return false;
    }
    device.last_lba = read_be32(capacity_data);
    device.block_size = read_be32(capacity_data + 4);
    if (device.block_size == 0u || device.block_size > 4096u || device.last_lba == 0xFFFFFFFFu) {
        device.present = false;
        starlix::drivers::serial::print("[EHCI][MSC] Unsupported capacity/block size; block device unavailable.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][MSC] Capacity ready: last LBA ");
    print_dec(device.last_lba);
    starlix::drivers::serial::print(", block size ");
    print_dec(device.block_size);
    starlix::drivers::serial::print(" bytes.\n");
    uint8_t first_block[4096]{};
    if (device.block_size <= sizeof(first_block) &&
        ehci_msc_read_blocks(dev_addr, 0u, 1u, first_block, sizeof(first_block))) {
        starlix::drivers::serial::print("[EHCI][MSC] READ(10) LBA 0 succeeded; block data reached kernel memory.\n");
    } else {
        starlix::drivers::serial::print("[EHCI][MSC] Capacity read succeeded, but READ(10) self-test failed.\n");
    }
    return true;
}

bool ehci_msc_read_blocks(uint8_t dev_addr, uint32_t lba, uint16_t block_count,
                          void* out_buffer, uint32_t out_capacity) {
    if (dev_addr == 0u || dev_addr > 127u || !out_buffer || block_count == 0u) return false;
    USB_MSC_DEVICE& device = usb_msc_devices[dev_addr];
    if (!starlix::drivers::ehci::recovery_policy::allow_block_io(
            device.present, static_cast<starlix::drivers::ehci::recovery_policy::DeviceState>(
                __atomic_load_n(&device.recovery_state, __ATOMIC_ACQUIRE)), controller_active) ||
        device.block_size == 0u) return false;
    const uint64_t bytes64 = static_cast<uint64_t>(device.block_size) * block_count;
    const uint64_t end_lba = static_cast<uint64_t>(lba) + block_count;
    if (bytes64 > 4096u || bytes64 > out_capacity || end_lba > (static_cast<uint64_t>(device.last_lba) + 1u)) return false;
    uint8_t cdb[10]{};
    cdb[0] = 0x28u; /* READ(10) */
    write_be32(cdb + 2, lba);
    write_be16(cdb + 7, block_count);
    return msc_bot_command(device, cdb, sizeof(cdb), true, out_buffer, static_cast<uint32_t>(bytes64));
}

bool ehci_msc_write_blocks(uint8_t dev_addr, uint32_t lba, uint16_t block_count,
                           const void* in_buffer, uint32_t in_capacity) {
    if (dev_addr == 0u || dev_addr > 127u || !in_buffer || block_count == 0u) return false;
    USB_MSC_DEVICE& device = usb_msc_devices[dev_addr];
    if (!starlix::drivers::ehci::recovery_policy::allow_block_io(
            device.present, static_cast<starlix::drivers::ehci::recovery_policy::DeviceState>(
                __atomic_load_n(&device.recovery_state, __ATOMIC_ACQUIRE)), controller_active) ||
        device.block_size == 0u) return false;
    const uint64_t bytes64 = static_cast<uint64_t>(device.block_size) * block_count;
    const uint64_t end_lba = static_cast<uint64_t>(lba) + block_count;
    if (bytes64 > 4096u || bytes64 > in_capacity || end_lba > (static_cast<uint64_t>(device.last_lba) + 1u)) return false;
    uint8_t cdb[10]{};
    cdb[0] = 0x2Au; /* WRITE(10) */
    write_be32(cdb + 2, lba);
    write_be16(cdb + 7, block_count);
    return msc_bot_command(device, cdb, sizeof(cdb), false,
                           const_cast<void*>(in_buffer), static_cast<uint32_t>(bytes64));
}

bool ehci_msc_synchronize_cache(uint8_t dev_addr) {
    if (dev_addr == 0u || dev_addr > 127u) return false;
    USB_MSC_DEVICE& device = usb_msc_devices[dev_addr];
    if (!starlix::drivers::ehci::recovery_policy::allow_block_io(
            device.present, static_cast<starlix::drivers::ehci::recovery_policy::DeviceState>(
                __atomic_load_n(&device.recovery_state, __ATOMIC_ACQUIRE)), controller_active)) return false;
    uint8_t cdb[10]{};
    cdb[0] = 0x35u; /* SYNCHRONIZE CACHE(10); a failed command is never treated as a flush. */
    return msc_bot_command(device, cdb, sizeof(cdb), false, nullptr, 0u);
}

static void ehci_hid_forget_endpoint(uint32_t endpoint_id) {
    if (endpoint_id == 0u) return;
    for (uint32_t i = 0u; i < EHCI_HID_BINDING_CAPACITY; ++i) {
        EHCIBoundHIDEndpoint& slot = ehci_hid_endpoints[i];
        if (slot.active && slot.endpoint_id == endpoint_id) {
            slot = EHCIBoundHIDEndpoint{};
            return;
        }
    }
}

static bool bind_hid_interfaces(uint8_t dev_addr, const uint8_t* descriptors,
                                uint16_t total_length) {
    if (!descriptors || total_length < 9u || dev_addr == 0u || dev_addr > 127u)
        return false;
    using Candidate = starlix::drivers::ehci::hid_input::BootInterfaceCandidate;
    Candidate candidates[starlix::drivers::ehci::hid_input::BOOT_INTERFACE_CAPACITY]{};
    uint32_t candidate_count = 0u;
    if (!starlix::drivers::ehci::hid_input::parse_boot_interfaces(
            descriptors, total_length, candidates,
            starlix::drivers::ehci::hid_input::BOOT_INTERFACE_CAPACITY,
            &candidate_count, usb_device_uses_uhci[dev_addr] ? 255u : 14u,
            (usb_device_uses_uhci[dev_addr] && companion_enumeration_low_speed) ? 10u : 1u)) return false;
    starlix::drivers::serial::print("[USB][HID] Parsed boot input interface candidates=");
    print_dec(candidate_count);
    starlix::drivers::serial::print(".\n");

    bool any_bound = false;
    for (uint32_t n = 0u; n < candidate_count; ++n) {
        Candidate& candidate = candidates[n];
        if (!candidate.active || !candidate.endpoint_found) continue;
        EHCIBoundHIDEndpoint* free_slot = nullptr;
        for (uint32_t i = 0u; i < EHCI_HID_BINDING_CAPACITY; ++i) {
            if (!ehci_hid_endpoints[i].active) { free_slot = &ehci_hid_endpoints[i]; break; }
        }
        if (!free_slot) break;

        /* Boot-protocol class request; no report-descriptor interpreter is
           needed for the standard 8-byte keyboard and 3/4-byte mouse reports. */
        USB_SETUP_PACKET set_protocol{0x21u, 0x0Bu, 0u,
            candidate.interface_number, 0u};
        if (!ehci_control_transfer(dev_addr, &set_protocol, nullptr, 0u)) {
            starlix::drivers::serial::print("[EHCI][HID] SET_PROTOCOL(Boot) failed; interface left unbound.\n");
            continue;
        }
        USB_SETUP_PACKET set_idle{0x21u, 0x0Au, 0u,
            candidate.interface_number, 0u};
        (void)ehci_control_transfer(dev_addr, &set_idle, nullptr, 0u);

        uint32_t endpoint_id = 0u;
        if (!ehci_periodic_interrupt_open(dev_addr, candidate.endpoint_address,
                candidate.max_packet_size, candidate.interval, &endpoint_id)) {
            starlix::drivers::serial::print("[USB][HID] Could not open interrupt-IN endpoint; interface left unbound.\n");
            continue;
        }
        *free_slot = EHCIBoundHIDEndpoint{};
        free_slot->active = true;
        free_slot->keyboard = candidate.keyboard;
        free_slot->device_address = dev_addr;
        free_slot->interface_number = candidate.interface_number;
        free_slot->endpoint_address = candidate.endpoint_address;
        free_slot->interval = candidate.interval;
        free_slot->max_packet_size = candidate.max_packet_size;
        free_slot->endpoint_id = endpoint_id;
        free_slot->device_generation = usb_device_generation[dev_addr];
        any_bound = true;
        starlix::drivers::serial::print(candidate.keyboard
            ? "[USB][HID] USB boot keyboard bound to persistent interrupt-IN schedule.\n"
            : "[USB][HID] USB boot mouse bound to persistent interrupt-IN schedule.\n");
    }
    return any_bound;
}

static bool bind_generic_hid_mouse_interfaces(uint8_t dev_addr,
                                               const uint8_t* descriptors,
                                               uint16_t total_length) {
    using namespace starlix::drivers::ehci::hid_policy;
    if(!descriptors||total_length<9u||dev_addr==0u||dev_addr>127u)return false;
    struct Candidate { uint8_t interface_number,endpoint,interval;uint16_t packet,report_length; };
    Candidate candidates[8]{};uint8_t count=0u;
    bool current=false;Candidate item{};bool have_hid_descriptor=false,have_endpoint=false;
    auto commit=[&]() {
        if(current&&have_hid_descriptor&&have_endpoint&&count<8u)candidates[count++]=item;
    };
    uint16_t off=descriptors[0];
    while(static_cast<uint32_t>(off)+2u<=total_length) {
        const uint8_t len=descriptors[off],type=descriptors[off+1u];
        if(len<2u||static_cast<uint32_t>(off)+len>total_length)return false;
        if(type==USB_DESC_INTERFACE) {
            commit();current=false;have_hid_descriptor=false;have_endpoint=false;
            if(len<9u)return false;
            if(descriptors[off+5u]==0x03u&&descriptors[off+6u]==0u) {
                current=true;item=Candidate{descriptors[off+2u],0u,0u,0u,0u};
            }
        } else if(current&&type==0x21u) {
            HidDescriptorSummary summary{};
            if(validate_hid_descriptor(descriptors+off,len,&summary)) {
                item.report_length=summary.report_length;have_hid_descriptor=true;
            }
        } else if(current&&type==USB_DESC_ENDPOINT&&len>=7u) {
            const uint8_t endpoint=descriptors[off+2u],attributes=descriptors[off+3u];
            const uint16_t raw_packet=static_cast<uint16_t>(descriptors[off+4u]|
                (static_cast<uint16_t>(descriptors[off+5u])<<8u));
            const uint16_t packet=static_cast<uint16_t>(raw_packet&0x07FFu);
            const uint8_t interval=descriptors[off+6u];
            const uint8_t min_interval=(usb_device_uses_uhci[dev_addr]&&companion_enumeration_low_speed)?10u:1u;
            const uint8_t max_interval=usb_device_uses_uhci[dev_addr]?255u:14u;
            if((endpoint&0x80u)&&(endpoint&0x70u)==0u&&(endpoint&0x0Fu)!=0u&&
               (attributes&3u)==3u&&(raw_packet&0xF800u)==0u&&packet>=3u&&packet<=64u&&
               interval>=min_interval&&interval<=max_interval) {
                item.endpoint=endpoint;item.packet=packet;item.interval=interval;have_endpoint=true;
            }
        }
        off=static_cast<uint16_t>(off+len);
    }
    commit();

    bool any_bound=false;
    for(uint8_t n=0;n<count;++n) {
        const Candidate& candidate=candidates[n];
        if(candidate.report_length==0u||candidate.report_length>512u) {
            starlix::drivers::serial::print("[USB][HID] Generic interface report descriptor length outside 1..512; rejected.\n");continue;
        }
        uint8_t report_descriptor[512]{};
        USB_SETUP_PACKET get_report{0x81u,USB_REQ_GET_DESCRIPTOR,
            static_cast<uint16_t>(0x2200u),candidate.interface_number,candidate.report_length};
        if(!ehci_control_transfer(dev_addr,&get_report,report_descriptor,candidate.report_length)) {
            starlix::drivers::serial::print("[USB][HID] Generic HID report descriptor request failed.\n");continue;
        }
        GenericMouseDescriptor layout{};
        if(!parse_generic_mouse_descriptor(report_descriptor,candidate.report_length,&layout)) {
            starlix::drivers::serial::print("[USB][HID] Generic HID descriptor malformed or mouse layout unsupported; interface rejected.\n");continue;
        }
        EHCIBoundHIDEndpoint* slot=nullptr;
        for(uint32_t i=0;i<EHCI_HID_BINDING_CAPACITY;++i)
            if(!ehci_hid_endpoints[i].active){slot=&ehci_hid_endpoints[i];break;}
        if(!slot)break;
        uint32_t endpoint_id=0u;
        if(!ehci_periodic_interrupt_open(dev_addr,candidate.endpoint,candidate.packet,
                                         candidate.interval,&endpoint_id)) {
            starlix::drivers::serial::print("[USB][HID] Generic mouse interrupt-IN schedule rejected.\n");continue;
        }
        *slot=EHCIBoundHIDEndpoint{};slot->active=true;slot->generic_mouse=true;
        slot->device_address=dev_addr;slot->interface_number=candidate.interface_number;
        slot->endpoint_address=candidate.endpoint;slot->interval=candidate.interval;
        slot->max_packet_size=candidate.packet;slot->endpoint_id=endpoint_id;
        slot->device_generation=usb_device_generation[dev_addr];slot->generic_mouse_layout=layout;
        starlix::drivers::serial::print("[USB][HID] Generic mouse report descriptor acquired bytes=");
        print_dec(candidate.report_length);starlix::drivers::serial::print(" interface=");
        print_dec(candidate.interface_number);starlix::drivers::serial::print(" reports=");
        print_dec(layout.report_count);starlix::drivers::serial::print(" endpoint=0x");
        print_hex16(candidate.endpoint);starlix::drivers::serial::print(" packet=");
        print_dec(candidate.packet);starlix::drivers::serial::print(" interval=");
        print_dec(candidate.interval);starlix::drivers::serial::print(".\n");
        for(uint8_t report_index=0u;report_index<layout.report_count;++report_index) {
            const GenericMouseReportLayout& r=layout.reports[report_index];
            starlix::drivers::serial::print("[USB][HID] Mouse layout ID=");
            print_dec(r.report_id);starlix::drivers::serial::print(" bits=");
            print_dec(r.report_bits);starlix::drivers::serial::print(" buttons=");
            print_dec(r.button_count);starlix::drivers::serial::print(" X-offset=");
            print_dec(r.x_offset);starlix::drivers::serial::print(" X-width=");print_dec(r.x_bits);
            starlix::drivers::serial::print(r.x_signed?" signed Y-offset=":" unsigned Y-offset=");
            print_dec(r.y_offset);starlix::drivers::serial::print(" Y-width=");print_dec(r.y_bits);
            starlix::drivers::serial::print(r.y_signed?" signed":" unsigned");
            if(r.has_wheel){starlix::drivers::serial::print(" wheel-offset=");
                print_dec(r.wheel_offset);starlix::drivers::serial::print("/");
                print_dec(r.wheel_bits);starlix::drivers::serial::print(r.wheel_signed?" signed":" unsigned");}
            else starlix::drivers::serial::print(" wheel=absent");
            starlix::drivers::serial::print(".\n");
        }
        any_bound=true;
    }
    return any_bound;
}

static uint8_t allocate_usb_device_address() {
    /* USB addresses 1..127 are reusable once the old device disconnects. */
    for (uint16_t attempt = 0u; attempt < 127u; ++attempt) {
        uint8_t candidate = next_device_address;
        if (candidate == 0u || candidate > 127u) candidate = 1u;
        next_device_address = (candidate == 127u) ? 1u : static_cast<uint8_t>(candidate + 1u);
        if (!usb_device_address_in_use[candidate]) {
            usb_device_address_in_use[candidate] = true;
            usb_device_generation[candidate] =
                starlix::drivers::ehci::device_lifetime::next_generation(
                    usb_device_generation[candidate]);
            return candidate;
        }
    }
    return 0u;
}

static void enumerate_usb_device(uint32_t port, bool high_speed) {
    uint8_t first_eight[8] = {};
    USB_SETUP_PACKET get_first_descriptor{
        0x80u, USB_REQ_GET_DESCRIPTOR,
        static_cast<uint16_t>(USB_DESC_DEVICE << 8), 0u, 8u
    };
    if (!ehci_control_transfer(0, &get_first_descriptor, first_eight, sizeof(first_eight))) {
        starlix::drivers::serial::print("[EHCI] GET_DESCRIPTOR (first 8 bytes) failed at address 0.\n");
        return;
    }
    if (!starlix::drivers::ehci::usb_core::validate_device_descriptor_prefix(
            first_eight, sizeof(first_eight), high_speed)) {
        starlix::drivers::serial::print("[USB] Invalid endpoint-zero descriptor; device skipped.\n");
        return;
    }
    const bool via_uhci = !high_speed;
    if (via_uhci) starlix::drivers::uhci::uhci_set_device(0u, companion_enumeration_low_speed, first_eight[7]);

    const uint8_t assigned_address = allocate_usb_device_address();
    if (assigned_address == 0u) {
        starlix::drivers::serial::print("[EHCI] USB device address space exhausted.\n");
        return;
    }
    USB_SETUP_PACKET set_address{
        0x00u, USB_REQ_SET_ADDRESS, assigned_address, 0u, 0u
    };
    if (!ehci_control_transfer(0, &set_address, nullptr, 0)) {
        usb_device_address_in_use[assigned_address] = false;
        starlix::drivers::serial::print("[EHCI] SET_ADDRESS failed.\n");
        return;
    }

    /* Track the address before the post-SET_ADDRESS delay so disconnect cleanup
       can release it even if the delay service is unavailable. */
    last_enumerated_address = 0u;
    if (!hub_child_enumeration_active) port_device_address[port] = assigned_address;
    device_ep0_max_packet[assigned_address] = first_eight[7];
    usb_device_uses_uhci[assigned_address] = via_uhci;
    if (via_uhci) {
        starlix::drivers::uhci::uhci_set_device(assigned_address, companion_enumeration_low_speed, first_eight[7]);
        if (!hub_child_enumeration_active)
            starlix::drivers::uhci::uhci_set_port_device_address(static_cast<uint8_t>(port), assigned_address);
    }
    usb_device_lifecycle_state[assigned_address] = static_cast<uint8_t>(
        starlix::drivers::ehci::device_lifetime::State::Enumerating);
    if (!hub_child_enumeration_active)
        enumerated_port_mask |= static_cast<uint16_t>(1u << port);
    if (!delay_ms(2)) {
        starlix::drivers::serial::print("[EHCI] PIT delay unavailable after SET_ADDRESS.\n");
        return;
    }

    starlix::drivers::serial::print("[EHCI] USB device address assigned: ");
    print_dec(assigned_address);
    starlix::drivers::serial::print("\n");

    uint8_t descriptor_bytes[18] = {};
    USB_SETUP_PACKET get_device_descriptor{
        0x80u, USB_REQ_GET_DESCRIPTOR,
        static_cast<uint16_t>(USB_DESC_DEVICE << 8), 0u, 18u
    };
    if (!ehci_control_transfer(assigned_address, &get_device_descriptor,
                               descriptor_bytes, sizeof(descriptor_bytes)) ||
        !starlix::drivers::ehci::usb_core::validate_device_descriptor(
            descriptor_bytes, sizeof(descriptor_bytes), high_speed)) {
        starlix::drivers::serial::print("[USB] Full device descriptor request failed.\n");
        return;
    }

    const uint16_t vendor_id = static_cast<uint16_t>(descriptor_bytes[8] |
        (static_cast<uint16_t>(descriptor_bytes[9]) << 8));
    const uint16_t product_id = static_cast<uint16_t>(descriptor_bytes[10] |
        (static_cast<uint16_t>(descriptor_bytes[11]) << 8));
    starlix::drivers::serial::print(via_uhci ? "[UHCI][USB] Enumerated VID:0x" : "[EHCI][USB] Enumerated VID:0x"); print_hex16(vendor_id);
    starlix::drivers::serial::print(" PID:0x"); print_hex16(product_id);
    starlix::drivers::serial::print("\n");

    uint8_t configuration_header[9] = {};
    USB_SETUP_PACKET get_config_header{
        0x80u, USB_REQ_GET_DESCRIPTOR,
        static_cast<uint16_t>(USB_DESC_CONFIGURATION << 8), 0u, 9u
    };
    if (!ehci_control_transfer(assigned_address, &get_config_header,
                               configuration_header, sizeof(configuration_header))) {
        starlix::drivers::serial::print("[EHCI] Configuration descriptor header unavailable.\n");
        return;
    }

    const uint16_t total_length = static_cast<uint16_t>(configuration_header[2] |
        (static_cast<uint16_t>(configuration_header[3]) << 8));
    const uint8_t configuration_value = configuration_header[5];
    if (total_length < sizeof(USB_CONFIG_DESCRIPTOR) || total_length > sizeof(configuration_buffer) ||
        configuration_value == 0u) {
        starlix::drivers::serial::print("[EHCI] Unsupported or invalid configuration descriptor.\n");
        return;
    }

    mem_zero(configuration_buffer, sizeof(configuration_buffer));
    USB_SETUP_PACKET get_full_configuration{
        0x80u, USB_REQ_GET_DESCRIPTOR,
        static_cast<uint16_t>(USB_DESC_CONFIGURATION << 8), 0u, total_length
    };
    if (!ehci_control_transfer(assigned_address, &get_full_configuration,
                               configuration_buffer, total_length) ||
        !validate_configuration_descriptor(configuration_buffer, total_length)) {
        starlix::drivers::serial::print("[EHCI] Full configuration descriptor validation failed.\n");
        return;
    }

    USB_SETUP_PACKET set_configuration{
        0x00u, USB_REQ_SET_CONFIGURATION, configuration_value, 0u, 0u
    };
    if (ehci_control_transfer(assigned_address, &set_configuration, nullptr, 0)) {
        last_enumerated_address = assigned_address;
        starlix::drivers::serial::print("[EHCI] Device configuration selected.\n");
        usb_device_lifecycle_state[assigned_address] = static_cast<uint8_t>(
            starlix::drivers::ehci::device_lifetime::State::Configured);
        starlix::drivers::ehci::usb_core::ConfigurationSummary summary{};
        if (!starlix::drivers::ehci::usb_core::parse_configuration(
                configuration_buffer, total_length, &summary)) {
            starlix::drivers::serial::print("[EHCI][USB] Configuration summary parse failed after validation.\n");
            return;
        }
        USBDeviceModelRecord& model = usb_device_model[assigned_address];
        mem_zero(&model, sizeof(model));
        model.present = true;
        model.info.device_address = assigned_address;
        model.info.root_port_number = static_cast<uint8_t>(port + 1u);
        model.info.lifecycle_state = static_cast<uint8_t>(
            starlix::drivers::ehci::device_model_policy::State::Configured);
        model.info.driver_binding = static_cast<uint8_t>(
            starlix::drivers::ehci::device_model_policy::Binding::Unbound);
        model.info.generation = usb_device_generation[assigned_address];
        model.info.vendor_id = vendor_id;
        model.info.product_id = product_id;
        model.info.usb_version_bcd = static_cast<uint16_t>(descriptor_bytes[2] | (static_cast<uint16_t>(descriptor_bytes[3]) << 8));
        model.info.device_version_bcd = static_cast<uint16_t>(descriptor_bytes[12] | (static_cast<uint16_t>(descriptor_bytes[13]) << 8));
        model.info.device_class = descriptor_bytes[4];
        model.info.device_subclass = descriptor_bytes[5];
        model.info.device_protocol = descriptor_bytes[6];
        model.info.configuration_value = configuration_value;
        model.info.interface_count = summary.distinct_interfaces;
        model.info.endpoint_count = summary.endpoint_count;
        starlix::drivers::serial::print("[USB][DESC] Interfaces=");
        print_dec(summary.distinct_interfaces);
        starlix::drivers::serial::print(" endpoints=");
        print_dec(summary.endpoint_count);
        starlix::drivers::serial::print(summary.has_hid_boot_input
            ? " boot-HID-interface=yes.\n" : " boot-HID-interface=no.\n");
        /* Enumeration diagnostics show class/endpoint metadata only; never HID payloads or typed input. */
        for (uint16_t off = configuration_buffer[0];
             static_cast<uint32_t>(off) + 9u <= total_length;) {
            const uint8_t length = configuration_buffer[off];
            const uint8_t type = configuration_buffer[off + 1u];
            if (length < 2u || static_cast<uint32_t>(off) + length > total_length) break;
            if (type == USB_DESC_INTERFACE && length >= 9u) {
                starlix::drivers::serial::print("[USB][DESC] Interface class/subclass/protocol=");
                print_hex16(configuration_buffer[off + 5u]); starlix::drivers::serial::print("/");
                print_hex16(configuration_buffer[off + 6u]); starlix::drivers::serial::print("/");
                print_hex16(configuration_buffer[off + 7u]); starlix::drivers::serial::print(" alt=");
                print_hex16(configuration_buffer[off + 3u]); starlix::drivers::serial::print(".\n");
            } else if (type == USB_DESC_ENDPOINT && length >= 7u) {
                starlix::drivers::serial::print("[USB][DESC] Endpoint address/type/packet/interval=");
                print_hex16(configuration_buffer[off + 2u]); starlix::drivers::serial::print("/");
                print_hex16(configuration_buffer[off + 3u] & 3u); starlix::drivers::serial::print("/");
                print_hex16(static_cast<uint16_t>(configuration_buffer[off + 4u] |
                    (static_cast<uint16_t>(configuration_buffer[off + 5u]) << 8u)));
                starlix::drivers::serial::print("/"); print_hex16(configuration_buffer[off + 6u]);
                starlix::drivers::serial::print(".\n");
            }
            off = static_cast<uint16_t>(off + length);
        }
        if (descriptor_bytes[4] == starlix::drivers::ehci::usb_core::CLASS_HUB ||
            summary.has_hub_interface) {
            uint8_t hub_descriptor[11]{};
            USB_SETUP_PACKET get_hub_descriptor{
                0xA0u, USB_REQ_GET_DESCRIPTOR,
                static_cast<uint16_t>(starlix::drivers::ehci::usb_core::DESC_HUB << 8),
                0u, static_cast<uint16_t>(sizeof(hub_descriptor))
            };
            starlix::drivers::ehci::usb_core::HubDescriptorSummary hub_summary{};
            if (!ehci_control_transfer(assigned_address, &get_hub_descriptor,
                                       hub_descriptor, sizeof(hub_descriptor)) ||
                !starlix::drivers::ehci::usb_core::validate_hub_descriptor(
                    hub_descriptor, sizeof(hub_descriptor), &hub_summary)) {
                starlix::drivers::serial::print("[EHCI][HUB] Hub-class device has an invalid/unavailable hub descriptor; downstream enumeration refused.\n");
                return;
            }
            const starlix::drivers::ehci::device_model_policy::Identity hub_identity{
                vendor_id, product_id, descriptor_bytes[4], true, true,
                summary.has_msc_bot_interface, summary.has_hid_boot_input, false};
            model.info.driver_binding = static_cast<uint8_t>(
                starlix::drivers::ehci::device_model_policy::choose_binding(hub_identity));
            const bool hub_tt = summary.hub_interface_protocol == 1u ||
                                summary.hub_interface_protocol == 2u;
            const auto hub_speed = via_uhci
                ? starlix::drivers::ehci::hub_policy::Speed::Full
                : starlix::drivers::ehci::hub_policy::Speed::High;
            const auto full_speed_route = starlix::drivers::ehci::hub_policy::route_device(
                false, true, hub_speed,
                starlix::drivers::ehci::hub_policy::Speed::Full, hub_tt, via_uhci, 2u);
            starlix::drivers::serial::print(via_uhci
                ? "[UHCI][HUB] Full-/low-speed hub descriptor validated; ports="
                : "[EHCI][HUB] High-speed hub descriptor validated; ports=");
            print_dec(hub_summary.port_count);
            if (full_speed_route == starlix::drivers::ehci::hub_policy::Route::CompanionHubChild)
                starlix::drivers::serial::print("; full-speed UHCI hub; polling its ports through real class control transfers.\n");
            else if (full_speed_route == starlix::drivers::ehci::hub_policy::Route::SplitTransactionRequired)
                starlix::drivers::serial::print("; TT advertised but split-transaction QH routing is not implemented; child enumeration is disabled.\n");
            else
                starlix::drivers::serial::print("; no supported TT route; low/full-speed children will be rejected.\n");
            if (via_uhci && !companion_enumeration_low_speed) {
                for (auto& hub : uhci_hubs) {
                    if (hub.active && hub.address != assigned_address) continue;
                    hub = UHCIHubRecord{};
                    hub.active = true;
                    hub.address = assigned_address;
                    hub.root_port = static_cast<uint8_t>(port);
                    hub.port_count = hub_summary.port_count > 7u ? 7u : hub_summary.port_count;
                    hub.power_good_2ms = hub_descriptor[2];
                    (void)uhci_hub_scan(hub, true);
                    break;
                }
            }
            return;
        }
        const bool msc_bound = bind_mass_storage(assigned_address, configuration_buffer, total_length);
        const bool hid_bound = bind_hid_interfaces(assigned_address, configuration_buffer, total_length);
        const bool generic_hid_bound = bind_generic_hid_mouse_interfaces(
            assigned_address, configuration_buffer, total_length);
        if (msc_bound) {
            const starlix::drivers::ehci::device_model_policy::Identity msc_identity{
                vendor_id, product_id, descriptor_bytes[4], false, false,
                summary.has_msc_bot_interface, summary.has_hid_boot_input, false};
            model.info.driver_binding = static_cast<uint8_t>(
                starlix::drivers::ehci::device_model_policy::choose_binding(msc_identity));
        } else if (hid_bound || generic_hid_bound) {
            const starlix::drivers::ehci::device_model_policy::Identity hid_identity{
                vendor_id, product_id, descriptor_bytes[4], false, false,
                false, true, false};
            model.info.driver_binding = static_cast<uint8_t>(
                starlix::drivers::ehci::device_model_policy::choose_binding(hid_identity));
        } else {
            starlix::drivers::serial::print("[EHCI] No supported active HID mouse/keyboard or USB Mass Storage BOT interface bound on this port.\n");
        }
    } else {
        starlix::drivers::serial::print("[EHCI] SET_CONFIGURATION failed.\n");
    }
}

/* Pure root-port state classification: kept separate from MMIO so the
   transition policy can be tested on a machine with no attached USB device. */
using EHCIPortAction = starlix::drivers::ehci::root_port_policy::Action;

static EHCIPortAction ehci_classify_root_port(bool connected, bool enumerated,
                                               bool companion_owned) {
    return starlix::drivers::ehci::root_port_policy::classify(
        connected, enumerated, companion_owned);
}

static bool ehci_root_port_debounce(uint32_t port, bool expected_connected) {
    if (!op_regs || port >= num_ports) return false;
    uint32_t first_status = op_regs->portsc[port];
    const bool first_connected = (first_status & PORT_CONNECT) != 0u;
    if (first_connected != expected_connected || !delay_ms(100u)) return false;
    uint32_t second_status = op_regs->portsc[port];
    acknowledge_port_changes(port, second_status);
    second_status = op_regs->portsc[port];
    const bool second_connected = (second_status & PORT_CONNECT) != 0u;
    return starlix::drivers::ehci::root_port_policy::debounce_confirmed(
        first_connected, second_connected, expected_connected);
}

static bool ehci_generation_is_pending(uint32_t current, uint32_t serviced) {
    return starlix::drivers::ehci::irq::generation_pending(current, serviced);
}

static bool ehci_hotplug_state_self_test() {
    struct PortCase {
        bool connected;
        bool enumerated;
        bool companion_owned;
        EHCIPortAction expected;
    };
    static constexpr PortCase port_cases[] = {
        {false, false, false, EHCIPortAction::Ignore},
        {true,  false, false, EHCIPortAction::Enumerate},
        {false, true,  false, EHCIPortAction::Disconnect},
        {true,  true,  false, EHCIPortAction::Ignore},
        {true,  false, true,  EHCIPortAction::Ignore},
        {false, true,  true,  EHCIPortAction::Disconnect}
    };
    for (const auto& test : port_cases) {
        if (ehci_classify_root_port(test.connected, test.enumerated,
                                    test.companion_owned) != test.expected)
            return false;
    }

    if (ehci_generation_is_pending(0u, 0u)) return false;
    if (!ehci_generation_is_pending(1u, 0u)) return false;
    if (!ehci_generation_is_pending(7u, 6u)) return false; /* event during service */
    if (!ehci_generation_is_pending(0u, 0xFFFFFFFFu)) return false; /* wraparound */
    return true;
}

static bool ehci_teardown_device_address(uint8_t address) {
    using starlix::drivers::ehci::device_lifetime::State;
    if (address == 0u || address > 127u || !usb_device_address_in_use[address]) return false;
    State state = static_cast<State>(usb_device_lifecycle_state[address]);
    if (state == State::Quarantined || state == State::Free) return false;
    if (state != State::Removing) {
        if (!starlix::drivers::ehci::device_lifetime::transition_allowed(state, State::Removing))
            return false;
        usb_device_lifecycle_state[address] = static_cast<uint8_t>(State::Removing);
    }
    if (__atomic_load_n(&usb_msc_devices[address].bot_lock, __ATOMIC_ACQUIRE) != 0u) {
        __atomic_store_n(&usb_msc_devices[address].recovery_state,
            static_cast<uint32_t>(starlix::drivers::ehci::recovery_policy::DeviceState::Offline),
            __ATOMIC_RELEASE);
        usb_device_lifecycle_state[address] = static_cast<uint8_t>(State::Quarantined);
        usb_device_model[address].present = false;
        usb_device_model[address].info.lifecycle_state = static_cast<uint8_t>(
            starlix::drivers::ehci::device_model_policy::State::Quarantined);
        starlix::drivers::serial::print("[EHCI][LIFETIME] Disconnect while MSC BOT owns command lock; address quarantined without clearing live lock.\n");
        return false;
    }

    uint32_t endpoint_ids[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
    uint32_t endpoint_count = 0u;
    for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
        if (periodic_endpoints[i].active && periodic_endpoints[i].device_address == address &&
            endpoint_count < EHCI_PERIODIC_ENDPOINT_CAPACITY)
            endpoint_ids[endpoint_count++] = periodic_endpoints[i].endpoint_id;
    }
    bool endpoints_closed = true;
    for (uint32_t i = 0; i < endpoint_count; ++i) {
        if (!ehci_periodic_interrupt_close(endpoint_ids[i])) {
            endpoints_closed = false;
        } else {
            ehci_hid_forget_endpoint(endpoint_ids[i]);
        }
    }

    uint32_t request_ids[EHCI_REQUEST_CAPACITY]{};
    uint32_t request_count = 0u;
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        EHCIRequestSlot& slot = ehci_request_slots[i];
        const uint32_t slot_state = __atomic_load_n(&slot.state, __ATOMIC_ACQUIRE);
        if (slot_state == static_cast<uint32_t>(EHCIRequestState::Free) ||
            slot.transfer.device_address != address) continue;
        const uint32_t id = __atomic_load_n(&slot.request_id, __ATOMIC_ACQUIRE);
        if (id != 0u && request_count < EHCI_REQUEST_CAPACITY) request_ids[request_count++] = id;
    }
    bool requests_drained = true;
    for (uint32_t i = 0; i < request_count; ++i) {
        EHCIRequestSnapshot snapshot{};
        if (!ehci_request_poll(request_ids[i], &snapshot)) { requests_drained = false; continue; }
        if (snapshot.state == EHCIRequestState::Queued) {
            if (!ehci_request_cancel(request_ids[i]) ||
                !ehci_request_poll(request_ids[i], &snapshot)) { requests_drained = false; continue; }
        }
        if (starlix::drivers::ehci::request_policy::terminal_state(
                static_cast<uint32_t>(snapshot.state))) {
            if (!ehci_request_release(request_ids[i])) requests_drained = false;
        } else {
            /* In-flight/transitioning requests make address reuse unsafe. */
            requests_drained = false;
        }
    }
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        const uint32_t slot_state = __atomic_load_n(&ehci_request_slots[i].state, __ATOMIC_ACQUIRE);
        if (slot_state != static_cast<uint32_t>(EHCIRequestState::Free) &&
            ehci_request_slots[i].transfer.device_address == address) requests_drained = false;
    }

    if (!starlix::drivers::ehci::device_lifetime::address_reusable(
            State::Removing, requests_drained, endpoints_closed)) {
        /* Remove user-visible device metadata even if DMA/schedule ownership could
           not be reclaimed. The address and backing DMA resources stay quarantined. */
        mem_zero(&usb_msc_devices[address], sizeof(usb_msc_devices[address]));
        mem_zero(bulk_data_toggle[address], sizeof(bulk_data_toggle[address]));
        device_ep0_max_packet[address] = 0u;
        usb_device_model[address].present = false;
        usb_device_model[address].info.lifecycle_state = static_cast<uint8_t>(
            starlix::drivers::ehci::device_model_policy::State::Quarantined);
        usb_device_lifecycle_state[address] = static_cast<uint8_t>(State::Quarantined);
        starlix::drivers::serial::print("[EHCI][LIFETIME] Device address quarantined; pending request or endpoint ownership prevents reuse.\n");
        return false;
    }

    mem_zero(&usb_msc_devices[address], sizeof(usb_msc_devices[address]));
    mem_zero(bulk_data_toggle[address], sizeof(bulk_data_toggle[address]));
    mem_zero(&usb_device_model[address], sizeof(usb_device_model[address]));
    device_ep0_max_packet[address] = 0u;
    usb_device_address_in_use[address] = false;
    usb_device_generation[address] =
        starlix::drivers::ehci::device_lifetime::next_generation(usb_device_generation[address]);
    usb_device_lifecycle_state[address] = static_cast<uint8_t>(State::Free);
    starlix::drivers::serial::print("[EHCI][LIFETIME] Requests drained, periodic endpoints closed and device address safely released.\n");
    return true;
}

static bool uhci_hub_port_request(uint8_t hub_address, uint8_t request,
                                  uint16_t value, uint8_t port,
                                  void* data, uint16_t length) {
    USB_SETUP_PACKET setup{static_cast<uint8_t>(length ? 0xA3u : 0x23u), request,
        value, static_cast<uint16_t>(port + 1u), length};
    return ehci_control_transfer(hub_address, &setup, data, length);
}

static void uhci_hub_forget_root_port(uint8_t root_port) {
    for (auto& hub : uhci_hubs) {
        if (!hub.active || hub.root_port != root_port) continue;
        for (uint8_t p = 0; p < hub.port_count; ++p) {
            const uint8_t child = hub.child_address[p];
            if (child) {
                (void)ehci_teardown_device_address(child);
                (void)starlix::drivers::uhci::uhci_forget_device(child);
                hub.child_address[p] = 0u;
            }
        }
        hub.active = false;
    }
}

/* Full-speed UHCI hubs are polled using USB hub class GET_STATUS requests.
   Child transfers continue through the same UHCI control/periodic schedule and
   common address, descriptor, HID and input path as root-port devices. */
static bool uhci_hub_scan(UHCIHubRecord& hub, bool force) {
    if (!hub.active || !starlix::drivers::uhci::uhci_is_active()) return false;
    const uint64_t now = starlix::cpu::timer_is_ready()
        ? starlix::cpu::timer_now_ms() : 0u;
    if (!force && now != 0u && hub.last_poll_ms != 0u &&
        now - hub.last_poll_ms < 250u) return false;
    hub.last_poll_ms = now;
    bool changed = false;
    for (uint8_t p = 0; p < hub.port_count; ++p) {
        uint8_t status[4]{};
        if (!uhci_hub_port_request(hub.address, 0u, 0u, p,
                                   status, sizeof(status))) continue;
        const auto decoded_status = starlix::drivers::ehci::hub_policy::decode_port_status(status);
        const uint16_t port_change = decoded_status.changes;
        static constexpr uint8_t change_features[5] = {16u, 17u, 18u, 19u, 20u};
        for (uint8_t bit = 0; bit < 5u; ++bit)
            if (port_change & (1u << bit))
                (void)uhci_hub_port_request(hub.address, 1u,
                    change_features[bit], p, nullptr, 0u);
        const uint8_t old_address = hub.child_address[p];
        const auto action = starlix::drivers::ehci::hub_policy::port_action(
            decoded_status, old_address != 0u);
        if (action == starlix::drivers::ehci::hub_policy::PortAction::Detach) {
            if (old_address) {
                const bool released = ehci_teardown_device_address(old_address);
                if (released) (void)starlix::drivers::uhci::uhci_forget_device(old_address);
                hub.child_address[p] = 0u;
                changed = true;
                starlix::drivers::serial::print(released
                    ? "[UHCI][HUB] Child detached; USB address and endpoint lifetime released.\n"
                    : "[UHCI][HUB] Child detached; unsafe endpoint ownership quarantined.\n");
            }
            continue;
        }
        if (action != starlix::drivers::ehci::hub_policy::PortAction::Attach) continue;
        const bool low_speed = decoded_status.low_speed;
        if (decoded_status.high_speed) {
            starlix::drivers::serial::print("[UHCI][HUB] High-speed child on UHCI hub rejected safely.\n");
            continue;
        }
        if (!uhci_hub_port_request(hub.address, 3u, 8u, p, nullptr, 0u)) continue;
        uint32_t power_delay = static_cast<uint32_t>(hub.power_good_2ms) * 2u;
        if (power_delay < 20u) power_delay = 20u;
        if (power_delay > 500u) power_delay = 500u;
        if (!delay_ms(power_delay) ||
            !uhci_hub_port_request(hub.address, 3u, 4u, p, nullptr, 0u) ||
            !delay_ms(50u)) continue;
        (void)uhci_hub_port_request(hub.address, 1u, 20u, p, nullptr, 0u);
        mem_zero(status, sizeof(status));
        if (!uhci_hub_port_request(hub.address, 0u, 0u, p,
                                   status, sizeof(status))) continue;
        const uint16_t after_reset = static_cast<uint16_t>(status[0] |
            (static_cast<uint16_t>(status[1]) << 8u));
        if ((after_reset & 3u) != 3u) {
            starlix::drivers::serial::print("[UHCI][HUB] Child port reset did not produce connected+enabled state.\n");
            continue;
        }
        companion_enumeration_active = true;
        companion_enumeration_low_speed = low_speed;
        hub_child_enumeration_active = true;
        last_enumerated_address = 0u;
        starlix::drivers::uhci::uhci_begin_enumeration(low_speed);
        starlix::drivers::uhci::uhci_set_device(0u, low_speed, 8u);
        starlix::drivers::serial::print("[USB][ROUTE] UHCI hub child enumeration started; speed=");
        starlix::drivers::serial::print(low_speed ? "low\n" : "full\n");
        enumerate_usb_device(hub.root_port, false);
        const uint8_t child_address = last_enumerated_address;
        starlix::drivers::uhci::uhci_end_enumeration();
        hub_child_enumeration_active = false;
        companion_enumeration_active = false;
        if (child_address) {
            hub.child_address[p] = child_address;
            changed = true;
            starlix::drivers::serial::print("[UHCI][HUB] Child configured on downstream port ");
            print_dec(p + 1u);
            starlix::drivers::serial::print(".\n");
        }
    }
    return changed;
}

bool ehci_service_hub_ports() {
    bool changed = false;
    for (auto& hub : uhci_hubs) changed = uhci_hub_scan(hub, false) || changed;
    return changed;
}

void ehci_scan_ports() {
    if (!controller_active || !op_regs) return;

    for (uint32_t port = 0; port < num_ports; ++port) {
        const uint16_t port_mask = static_cast<uint16_t>(1u << port);
        uint32_t status = op_regs->portsc[port];
        acknowledge_port_changes(port, status);
        status = op_regs->portsc[port];

        const bool connected = (status & PORT_CONNECT) != 0u;
        const bool enumerated = (enumerated_port_mask & port_mask) != 0u;
        const bool companion_owned = (status & PORT_OWNER) != 0u;
        EHCIPortAction action =
            ehci_classify_root_port(connected, enumerated, companion_owned);

        if (action == EHCIPortAction::Disconnect || action == EHCIPortAction::Enumerate) {
            if (!ehci_root_port_debounce(port, connected)) {
                starlix::drivers::serial::print("[EHCI][PORT] Connection changed during 100-ms debounce; scan deferred.\n");
                continue;
            }
            status = op_regs->portsc[port];
            acknowledge_port_changes(port, status);
            status = op_regs->portsc[port];
            const bool stable_connected = (status & PORT_CONNECT) != 0u;
            const bool stable_enumerated = (enumerated_port_mask & port_mask) != 0u;
            const bool stable_companion = (status & PORT_OWNER) != 0u;
            action = ehci_classify_root_port(stable_connected, stable_enumerated, stable_companion);
            if (action == EHCIPortAction::Ignore) continue;
        }

        if (action == EHCIPortAction::Disconnect) {
            const uint8_t old_address = port_device_address[port];
            bool released = true;
            if (old_address != 0u && old_address < 128u)
                released = ehci_teardown_device_address(old_address);
            if (!released && old_address != 0u) {
                /* Keep the old address unavailable even when cleanup is unsafe. */
                starlix::drivers::serial::print("[EHCI][LIFETIME] Disconnect cleanup incomplete; address retained/quarantined.\n");
            }
            port_device_address[port] = 0u;
            enumerated_port_mask = static_cast<uint16_t>(enumerated_port_mask & ~port_mask);
            starlix::drivers::serial::print("[EHCI][HOTPLUG] Device disconnected from root port ");
            print_dec(port + 1u);
            starlix::drivers::serial::print(released
                ? "; device lifetime cleanup completed.\n"
                : "; unsafe resources retained and address not reused.\n");
            continue;
        }
        if (action == EHCIPortAction::Ignore) {
            if (connected && !enumerated && companion_owned)
                starlix::drivers::serial::print("[EHCI] Root port is owned by a companion controller.\n");
            continue;
        }

        starlix::drivers::serial::print("[EHCI] Device detected on root port ");
        print_dec(port + 1u);
        starlix::drivers::serial::print("\n");

        if ((hcs_params & HCS_PPC) && !(status & PORT_POWER)) {
            write_port_control(port, (status & (PORT_OWNER | PORT_ENABLE | PORT_SUSPEND)) | PORT_POWER, status & PORT_CHANGE_MASK);
            if (!delay_ms(100)) {
                starlix::drivers::serial::print("[EHCI] Could not safely power port: PIT delay unavailable.\n");
                continue;
            }
        }

        if (reset_root_port(port)) enumerate_usb_device(port, true);
    }
}

void ehci_companion_enumerate(uint8_t port, bool low_speed) {
    if (!controller_active || port >= 16u || companion_enumeration_active ||
        (enumerated_port_mask & static_cast<uint16_t>(1u << port)) != 0u) return;
    starlix::drivers::serial::print("[USB][ROUTE] UHCI companion enumeration started; speed=");
    starlix::drivers::serial::print(low_speed ? "low\n" : "full\n");
    companion_enumeration_active = true;
    companion_enumeration_low_speed = low_speed;
    starlix::drivers::uhci::uhci_begin_enumeration(low_speed);
    enumerate_usb_device(port, false);
    starlix::drivers::uhci::uhci_end_enumeration();
    companion_enumeration_active = false;
}

bool ehci_companion_disconnect(uint8_t port, uint8_t address) {
    if (port >= 16u || address == 0u || address > 127u ||
        port_device_address[port] != address) return false;
    starlix::drivers::serial::print("[UHCI][HOTPLUG] Disconnecting device address ");
    print_dec(address);
    starlix::drivers::serial::print(" from companion root port.\n");
    uhci_hub_forget_root_port(port);
    const bool released = ehci_teardown_device_address(address);
    if (released) (void)starlix::drivers::uhci::uhci_forget_device(address);
    port_device_address[port] = 0u;
    enumerated_port_mask = static_cast<uint16_t>(enumerated_port_mask &
        ~static_cast<uint16_t>(1u << port));
    return released;
}

static bool ehci_power_has_outstanding_requests() {
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        if (__atomic_load_n(&ehci_request_slots[i].state, __ATOMIC_ACQUIRE) !=
            static_cast<uint32_t>(EHCIRequestState::Free)) return true;
    }
    return false;
}

static bool ehci_power_has_active_bot_command() {
    for (uint32_t address = 1u; address < 128u; ++address) {
        if (__atomic_load_n(&usb_msc_devices[address].bot_lock, __ATOMIC_ACQUIRE) != 0u)
            return true;
    }
    return false;
}

bool ehci_root_port_suspend(uint8_t port_index) {
    if (!controller_active || !op_regs || port_index >= num_ports || port_index >= 16u)
        return false;
    const uint32_t status = op_regs->portsc[port_index];
    const auto decision = starlix::drivers::ehci::power_policy::suspend_admission(
        (status & PORT_CONNECT) != 0u, (status & PORT_ENABLE) != 0u,
        (status & PORT_OWNER) != 0u, (status & PORT_SUSPEND) != 0u,
        __atomic_load_n(&ehci_transfer_engine_busy, __ATOMIC_ACQUIRE) == 0u,
        __atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE) ==
            static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle),
        periodic_endpoint_active == 0u && __atomic_load_n(&periodic_endpoint_lock, __ATOMIC_ACQUIRE) == 0u,
        !ehci_power_has_outstanding_requests() && !ehci_power_has_active_bot_command());
    if (decision != starlix::drivers::ehci::power_policy::Result::Allowed) return false;
    write_port_control(port_index, (status & (PORT_POWER | PORT_OWNER | PORT_ENABLE)) | PORT_SUSPEND,
                       status & PORT_CHANGE_MASK);
    if (!delay_ms(5u)) return false;
    const uint32_t settled = op_regs->portsc[port_index];
    return starlix::drivers::ehci::power_policy::suspend_confirmed(
        (settled & PORT_CONNECT) != 0u, (settled & PORT_ENABLE) != 0u,
        (settled & PORT_SUSPEND) != 0u);
}

bool ehci_root_port_resume(uint8_t port_index) {
    if (!controller_active || !op_regs || port_index >= num_ports || port_index >= 16u)
        return false;
    const uint32_t status = op_regs->portsc[port_index];
    const auto decision = starlix::drivers::ehci::power_policy::resume_admission(
        (status & PORT_CONNECT) != 0u, (status & PORT_ENABLE) != 0u,
        (status & PORT_OWNER) != 0u, (status & PORT_SUSPEND) != 0u);
    if (decision != starlix::drivers::ehci::power_policy::Result::Allowed) return false;
    const uint32_t stable_control = status & (PORT_POWER | PORT_OWNER | PORT_ENABLE);
    write_port_control(port_index, stable_control | PORT_RESUME, status & PORT_CHANGE_MASK);
    if (!delay_ms(20u)) {
        write_port_control(port_index, stable_control, 0u);
        return false;
    }
    /* End resume signaling; EHCI clears PORT_SUSPEND as the port returns active. */
    write_port_control(port_index, stable_control, 0u);
    if (!delay_ms(2u)) return false;
    const uint32_t settled = op_regs->portsc[port_index];
    return starlix::drivers::ehci::power_policy::resume_confirmed(
        (settled & PORT_CONNECT) != 0u, (settled & PORT_SUSPEND) != 0u,
        (settled & PORT_RESUME) != 0u);
}

bool ehci_port_change_pending() {
    if (!controller_active || !op_regs) return false;
    if (ehci_irq_enabled) {
        const uint32_t current = __atomic_load_n(&ehci_port_change_generation, __ATOMIC_ACQUIRE);
        if (ehci_generation_is_pending(current, ehci_port_change_serviced_generation)) return true;
    }
    /* The EHCI root-port change bits are readable W1C status. When PCI legacy
       routing is unavailable, the PIT wakeup drives this bounded polling path. */
    for (uint32_t port = 0; port < num_ports; ++port) {
        if (starlix::drivers::ehci::hid_input::port_change_pending(
                op_regs->portsc[port], PORT_CHANGE_MASK)) return true;
    }
    return false;
}

bool ehci_service_port_changes() {
    if (!controller_active || !op_regs) return false;
    if (__atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE) !=
            static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle) ||
        __atomic_load_n(&ehci_transfer_engine_busy, __ATOMIC_ACQUIRE) != 0u) return false;
    bool polling_change = false;
    if (!ehci_irq_enabled) {
        for (uint32_t port = 0; port < num_ports; ++port) {
            if (starlix::drivers::ehci::hid_input::port_change_pending(
                    op_regs->portsc[port], PORT_CHANGE_MASK)) {
                polling_change = true;
                break;
            }
        }
        if (!polling_change) return false;
    } else {
        const uint32_t observed = __atomic_load_n(&ehci_port_change_generation, __ATOMIC_ACQUIRE);
        if (!ehci_generation_is_pending(observed, ehci_port_change_serviced_generation)) return false;
        /* Preserve a newer event arriving during enumeration for the next pass. */
        ehci_port_change_serviced_generation = observed;
    }
    ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::PortRescans);
    starlix::drivers::serial::print(polling_change
        ? "[EHCI][HOTPLUG] Polled root-port change; rescanning ports.\n"
        : "[EHCI][HOTPLUG] Root-port change IRQ; rescanning ports.\n");
    ehci_scan_ports();
    return true;
}

bool ehci_init() {
    if (!starlix::drivers::ehci::release::self_test()) {
        starlix::drivers::serial::print("[EHCI][RELEASE] Release manifest self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][RELEASE] Beta release manifest self-test passed (driver/API 1.0; fail-closed DMA/class defaults; hardware sign-off gate retained).\n");
    const bool dma_ready_before_ehci_preflight = starlix::mem::phys_allocator_ready();
    if (controller_dma_quarantined) {
        starlix::drivers::serial::print("[EHCI] REFUSED: prior controller halt failed; DMA memory remains quarantined until reboot.\n");
        return false;
    }
    if (controller_active) {
        starlix::drivers::serial::print("[EHCI] Controller is already initialized.\n");
        return true;
    }
    if (!starlix::drivers::ehci::irq::CauseLatch::self_test()) {
        starlix::drivers::serial::print("[EHCI][IRQ] Atomic event-latch policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][IRQ] Atomic event-latch self-test passed (coalescing, drain, counters and generation wrap).\n");
    if (!starlix::drivers::ehci::irq::deferred_dispatch_self_test()) {
        starlix::drivers::serial::print("[EHCI][WORK] Deferred IRQ dispatch self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][WORK] Deferred IRQ dispatch self-test passed (completion, error, port-change and IAA routing).\n");
    if (!starlix::drivers::ehci::request_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][REQ] Request admission/terminal policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][REQ] Admission, cancellation, completion and release policy self-test passed.\n");
    if (!starlix::drivers::ehci::async_request_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][ASYNC] Nonblocking request policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print(
        "[EHCI][ASYNC] Nonblocking request state-machine self-test passed (monotonic deadlines, completion, cancellation and IAA safe-point).\n");
    if (!ehci_lifecycle_policy_self_test()) return false;
    if (!ehci_request_queue_init() || !ehci_request_lifecycle_self_test()) {
        starlix::drivers::serial::print("[EHCI][REQ] Request lifecycle self-test FAILED.\n");
        return false;
    }
    if (!starlix::drivers::ehci::usb_core::configuration_self_test()) {
        starlix::drivers::serial::print("[EHCI][USB] Descriptor core self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][USB] Descriptor core self-test passed (bounded configuration parsing and descriptor validation).\n");
    if (!starlix::drivers::ehci::root_port_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][PORT] Root-port debounce policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][PORT] Root-port state/debounce policy self-test passed.\n");
    if (!starlix::drivers::ehci::hub_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][HUB] Hub descriptor/routing policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][HUB] Hub descriptor and speed-routing policy self-test passed (unsupported TT routes rejected).\n");
    if (!starlix::drivers::ehci::device_lifetime::self_test()) {
        starlix::drivers::serial::print("[EHCI][LIFETIME] Device lifetime policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][LIFETIME] Device address generation/teardown policy self-test passed.\n");
    if (!starlix::drivers::ehci::device_model_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][DEVICE] Device-model/driver-binding policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][DEVICE] Device model and driver-binding policy self-test passed.\n");
    if (!starlix::drivers::ehci::recovery_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][RECOVERY] Error-recovery/BOT policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][RECOVERY] Error recovery and serialized BOT policy self-test passed.\n");
    if (!starlix::drivers::ehci::power_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][POWER] Root-port power-management policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][POWER] Root-port suspend/resume admission policy self-test passed.\n");
    if (!starlix::drivers::ehci::storage_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][STORAGE] Block/GPT/write-safety policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][STORAGE] Bounded block I/O, GPT backup and fixed-size write policy self-test passed.\n");
    if (!starlix::drivers::ehci::hid_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][HID] Descriptor/report/boot-input policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][HID] HID descriptor/report and boot keyboard/mouse policy self-test passed.\n");
    if (!starlix::drivers::ehci::hid_input::self_test()) {
        starlix::drivers::serial::print("[EHCI][HID] Input event queue/key-transition/mouse-report self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][HID] Input event queue, key transitions and mouse-report self-test passed.\n");
    if (!starlix::drivers::ehci::cdc_acm_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][CDC] CDC ACM/serial-buffer policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][CDC] ACM line coding, bulk endpoints and bounded serial ring self-test passed.\n");
    if (!starlix::drivers::ehci::usbnet_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][NET] USB network frame policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][NET] USB network class/MAC/frame-ownership policy self-test passed.\n");
    if (!starlix::drivers::ehci::audio_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][AUDIO] Audio endpoint/format policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][AUDIO] High-speed audio format, feedback and bounded packet queue policy self-test passed.\n");
    if (!starlix::drivers::ehci::video_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][VIDEO] UVC payload/frame policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][VIDEO] UVC format, payload-header and frame-bound policy self-test passed.\n");
    if (!starlix::drivers::ehci::printer_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][PRINTER] Printer descriptor/job policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][PRINTER] Printer interface, device ID, status and bounded job queue policy self-test passed.\n");
    if (!starlix::drivers::ehci::vendor_api_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][VENDOR] Vendor API allowlist/handle/request policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][VENDOR] Vendor API allowlist, generation handles and bounced-buffer policy self-test passed.\n");
    if (!starlix::drivers::ehci::class_registry::self_test()) {
        starlix::drivers::serial::print("[EHCI][CLASS] Extended class registry policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][CLASS] Extended USB class registry self-test passed (unsupported and privileged classes fail closed).\n");
    if (!starlix::drivers::ehci::security_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][SECURITY] Bounds/protocol hardening self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][SECURITY] Descriptor, transfer-range and BOT bounds self-test passed.\n");
    if (!starlix::drivers::ehci::regression_policy::self_test()) {
        starlix::drivers::serial::print("[EHCI][REGRESSION] Required-suite manifest self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][REGRESSION] Required-suite manifest self-test passed (missing or failed suites cannot pass).\n");
    if (!starlix::drivers::ehci::diagnostics::self_test()) {
        starlix::drivers::serial::print("[EHCI][DIAGNOSTICS] Counter semantics self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][DIAGNOSTICS] Runtime I/O/IRQ/error/latency counters self-test passed.\n");
    if (!ehci_hotplug_state_self_test()) {
        starlix::drivers::serial::print("[EHCI][HOTPLUG] State-machine self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][HOTPLUG] State-machine self-test passed (connect/disconnect/stable/companion/generation cases).\n");
    if (!periodic_schedule_policy_self_test()) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Multi-QH schedule policy self-test FAILED.\n");
        return false;
    }
    if (!starlix::drivers::ehci::isochronous::self_test()) {
        starlix::drivers::serial::print("[EHCI][ISO] iTD policy self-test FAILED.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][ISO] High-speed one-transaction iTD policy self-test passed.\n");
    starlix::drivers::serial::print("[EHCI][REQ] Request lifecycle self-test passed (queue/active ownership, cancel/release guards, terminal ID invalidation).\n");
    const bool dma_ready_after_request_test = starlix::mem::phys_allocator_ready();
    if (dma_ready_before_ehci_preflight && !dma_ready_after_request_test) {
        starlix::drivers::serial::print("[DMA-CHECK] Allocator lost ready state during EHCI request-queue preflight.\n");
    }
    controller_active = false;
    cap_regs = nullptr;
    op_regs = nullptr;
    ehci_irq_enabled = false;
    ehci_irq_line = 0xFFu;
    ehci_irq_event_latch.reset();
    num_ports = 0;
    hcs_params = 0;
    next_device_address = 1;
    enumerated_port_mask = 0;
    mem_zero(ehci_hid_endpoints, sizeof(ehci_hid_endpoints));
    ehci_hid_event_queue.clear();
    ehci_hid_transfer_errors = 0u;
    mem_zero(port_device_address, sizeof(port_device_address));
    mem_zero(usb_device_address_in_use, sizeof(usb_device_address_in_use));
    mem_zero(usb_device_lifecycle_state, sizeof(usb_device_lifecycle_state));
    mem_zero(usb_device_generation, sizeof(usb_device_generation));
    mem_zero(usb_device_model, sizeof(usb_device_model));
    __atomic_store_n(&ehci_port_change_generation, 0u, __ATOMIC_RELAXED);
    ehci_port_change_serviced_generation = 0u;
    mem_zero(device_ep0_max_packet, sizeof(device_ep0_max_packet));
    mem_zero(bulk_data_toggle, sizeof(bulk_data_toggle));
    mem_zero(usb_msc_devices, sizeof(usb_msc_devices));
    device_ep0_max_packet[0] = 64u;

    uint8_t bus = 0, slot = 0, function = 0;
    uint32_t bar0 = 0;
    if (!starlix::drivers::pci::pci_find_ehci(bus, slot, function, bar0)) {
        starlix::drivers::serial::print("[EHCI] No PCI EHCI controller (class 0C/03, ProgIF 20) found.\n");
        return false;
    }
    const uint32_t raw_bar0 = starlix::drivers::pci::pci_read(bus, slot, function, 0x10u);
    uint32_t validated_bar0 = 0u;
    if (!detail::decode_ehci_mmio_bar32(raw_bar0, bar0, &validated_bar0)) {
        starlix::drivers::serial::print("[EHCI] Refusing invalid/unsupported PCI BAR0 (requires 32-bit non-prefetchable MMIO).\n");
        return false;
    }
    bar0 = validated_bar0;
    starlix::drivers::serial::print("[EHCI] PCI MMIO BAR validated as 32-bit non-prefetchable memory.\n");
    if (dma_ready_after_request_test && !starlix::mem::phys_allocator_ready()) {
        starlix::drivers::serial::print("[DMA-CHECK] Allocator lost ready state during EHCI PCI discovery/BAR validation.\n");
    }
    if (!ehci_dma_storage_init()) {
        starlix::drivers::serial::print("[EHCI] DMA storage unavailable; controller initialization aborted.\n");
        return false;
    }
    if (!dma_sync_api_self_test()) {
        starlix::drivers::serial::print("[EHCI] DMA synchronization contract failed; controller initialization aborted.\n");
        return false;
    }

    starlix::drivers::serial::print("[EHCI] EHCI PCI function found; enabling memory space and bus mastering.\n");
    uint32_t command_status = starlix::drivers::pci::pci_read(bus, slot, function, 0x04);
    const uint32_t command = (command_status & 0xFFFFu) | 0x0006u;
    /* Upper 16 bits are written as zero to avoid clearing PCI W1C status bits. */
    starlix::drivers::pci::pci_write(bus, slot, function, 0x04, command);
    const uint32_t command_readback = starlix::drivers::pci::pci_read(
        bus, slot, function, 0x04) & 0xFFFFu;
    if ((command_readback & 0x0006u) != 0x0006u) {
        starlix::drivers::serial::print("[EHCI] PCI memory-space/bus-master enable readback failed.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI] PCI memory-space and bus-master bits verified.\n");

    cap_regs = reinterpret_cast<volatile EHCI_CAP_REGS*>(static_cast<uintptr_t>(bar0));
    if (cap_regs->caplength < sizeof(EHCI_CAP_REGS) || cap_regs->caplength > 0x40u) {
        starlix::drivers::serial::print("[EHCI] Invalid capability-register length.\n");
        cap_regs = nullptr;
        return false;
    }
    if (!detail::ehci_mmio_window_fits32(bar0, cap_regs->caplength,
                                         static_cast<uint32_t>(sizeof(EHCI_OP_REGS)))) {
        starlix::drivers::serial::print("[EHCI] Operational-register MMIO window wraps 32-bit address space.\n");
        cap_regs = nullptr;
        return false;
    }
    op_regs = reinterpret_cast<volatile EHCI_OP_REGS*>(
        static_cast<uintptr_t>(bar0 + static_cast<uint32_t>(cap_regs->caplength)));
    hcs_params = cap_regs->hcsparams;
    num_ports = hcs_params & HCS_N_PORTS_MASK;
    if (num_ports == 0 || num_ports > 15u) {
        starlix::drivers::serial::print("[EHCI] Unsupported root-port count.\n");
        cap_regs = nullptr;
        op_regs = nullptr;
        return false;
    }

    const uint32_t hcc_params_value = cap_regs->hccparams;
    if (hcc_params_value & HCC_64BIT) {
        starlix::drivers::serial::print("[EHCI] Controller supports 64-bit addressing; using 32-bit DMA segment 0.\n");
    }
    if (!release_legacy_ownership(bus, slot, function, hcc_params_value)) {
        starlix::drivers::serial::print("[EHCI] Refusing to initialize while BIOS owns the controller.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }

    /* Phase 7A: prove the firmware controller is fully quiescent before reset.
       HALTED alone is insufficient if either schedule-status bit remains active. */
    op_regs->usbintr = 0;
    op_regs->usbcmd &= ~(CMD_RUN | CMD_ASYNC_EN | CMD_PERIODIC_EN | CMD_ASYNC_ADVANCE);
    dma_write_barrier();

    const bool preflight_halted =
        wait_controller_status(STS_HALTED, STS_HALTED, 10000000u);
    const bool preflight_async_inactive = preflight_halted &&
        wait_controller_status(STS_ASYNC, 0u, 10000000u);
    const bool preflight_periodic_inactive = preflight_halted &&
        wait_controller_status(STS_PERIODIC, 0u, 10000000u);

    if (!starlix::drivers::ehci::lifecycle::status_confirms_quiescent(
            preflight_halted, preflight_async_inactive, preflight_periodic_inactive)) {
        controller_dma_quarantined = true;
        starlix::drivers::serial::print(
            "[EHCI] FATAL: startup could not prove HALTED with both schedules inactive; controller quarantined until reboot.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }

    op_regs->usbcmd = CMD_RESET;
    const bool reset_cleared =
        wait_register(&op_regs->usbcmd, CMD_RESET, 0u, 10000000u);
    const bool reset_halted = reset_cleared &&
        wait_controller_status(STS_HALTED, STS_HALTED, 10000000u);
    const bool reset_async_inactive = reset_halted &&
        wait_controller_status(STS_ASYNC, 0u, 10000000u);
    const bool reset_periodic_inactive = reset_halted &&
        wait_controller_status(STS_PERIODIC, 0u, 10000000u);

    if (!starlix::drivers::ehci::lifecycle::status_confirms_quiescent(
            reset_halted, reset_async_inactive, reset_periodic_inactive)) {
        controller_dma_quarantined = true;
        starlix::drivers::serial::print(
            "[EHCI] FATAL: reset did not establish a halted controller with inactive schedules; controller quarantined until reboot.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }

    uint32_t qh_address = 0, periodic_address = 0, setup_qtd_address = 0;
    uint32_t status_qtd_address = 0, bulk_qtd_address = 0;
    uint32_t setup_buffer_address = 0, data_qtd_pool_address = 0;
    uint32_t periodic_qh_address = 0, periodic_qtd_address = 0, periodic_buffer_address = 0;
    if (!dma_range32(async_head_qh, sizeof(*async_head_qh), 32u, qh_address) ||
        !dma_range32(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh), 32u, periodic_qh_address) ||
        !dma_range32(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd), 32u, periodic_qtd_address) ||
        !dma_range32(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, periodic_buffer_address) ||
        !dma_range32(periodic_frame_list, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, periodic_address) ||
        !dma_range32(setup_qtd_storage, sizeof(*setup_qtd_storage), 32u, setup_qtd_address) ||
        !dma_range32(data_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, data_qtd_pool_address) ||
        !dma_range32(status_qtd_storage, sizeof(*status_qtd_storage), 32u, status_qtd_address) ||
        !dma_range32(bulk_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, bulk_qtd_address) ||
        !dma_range32(setup_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, setup_buffer_address)) {
        starlix::drivers::serial::print("[EHCI] DMA structures are not addressable/aligned for EHCI.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }
    for (size_t i = 0; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
        uint32_t ignored = 0;
        if (!dma_range32(data_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, ignored) ||
            !dma_range32(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, ignored)) {
            starlix::drivers::serial::print("[EHCI] Transfer DMA page is not addressable/aligned.\n");
            op_regs = nullptr;
            cap_regs = nullptr;
            return false;
        }
    }

    mem_zero(async_head_qh, sizeof(*async_head_qh));
    mem_zero(async_request_qh, sizeof(*async_request_qh));
    async_request_qh->horizontal_link = LINK_TERMINATE;
    mem_zero(setup_qtd_storage, sizeof(*setup_qtd_storage));
    mem_zero(data_qtd_storage, EHCI_DMA_PAGE_SIZE);
    mem_zero(status_qtd_storage, sizeof(*status_qtd_storage));
    mem_zero(bulk_qtd_storage, EHCI_DMA_PAGE_SIZE);
    mem_zero(setup_dma_page, EHCI_DMA_PAGE_SIZE);
    mem_zero(periodic_interrupt_qh, sizeof(*periodic_interrupt_qh));
    mem_zero(periodic_interrupt_qtd, sizeof(*periodic_interrupt_qtd));
    mem_zero(periodic_interrupt_dma_page, EHCI_DMA_PAGE_SIZE);
    mem_zero(isochronous_itd_storage, sizeof(*isochronous_itd_storage));
    mem_zero(isochronous_dma_page, EHCI_DMA_PAGE_SIZE);
    zero_dma_pages(data_dma_pages);
    zero_dma_pages(bulk_dma_pages);
    for (unsigned i = 0; i < 1024; ++i) periodic_frame_list[i] = LINK_TERMINATE;
    dma_write_barrier();
    if (!periodic_frame_list_layout_self_test()) {
        starlix::drivers::serial::print("[EHCI][PERIODIC] Frame-list interval self-test failed.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }
    if (!periodic_bandwidth_self_test()) {
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }

    op_regs->ctrldssegment = 0; /* All descriptors/buffers supplied by this driver are below 4 GiB. */
    op_regs->periodiclistbase = periodic_address;
    op_regs->asynclistaddr = qh_address;
    async_head_qh->horizontal_link = qh_address | LINK_TYPE_QH;
    async_head_qh->ep_characteristics = make_qh_characteristics(0, 64);
    async_head_qh->ep_capabilities = (1u << 30); /* Mult=1; reserved zero is avoided. */
    async_head_qh->current_qtd = 0;
    async_head_qh->overlay.next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.token = 0;
    if (!sync_all_ehci_dma_for_device()) {
        starlix::drivers::serial::print("[EHCI][DMA] Final storage synchronization failed; controller remains halted.\n");
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }
    dma_write_barrier();

    clear_status(STS_W1C_MASK);
    op_regs->configflag = 1u; /* Route high-speed root ports to EHCI. */
    op_regs->usbintr = 0;     /* Phase 6 still uses bounded polling, not IRQs. */
    dma_write_barrier();
    op_regs->usbcmd = CMD_RUN | CMD_ASYNC_EN;
    dma_write_barrier();

    const bool running_halted_clear =
        wait_controller_status(STS_HALTED, 0u, 10000000u);
    const bool running_async_active = running_halted_clear &&
        wait_controller_status(STS_ASYNC, STS_ASYNC, 10000000u);

    if (!starlix::drivers::ehci::lifecycle::status_confirms_running(
            running_halted_clear, running_async_active)) {
        starlix::drivers::serial::print(
            "[EHCI] Run/async schedule did not become active; attempting verified stop.\n");
        op_regs->usbintr = 0u;
        op_regs->usbcmd &= ~(CMD_RUN | CMD_ASYNC_EN | CMD_PERIODIC_EN | CMD_ASYNC_ADVANCE);
        dma_write_barrier();

        const bool failed_start_halted =
            wait_controller_status(STS_HALTED, STS_HALTED, 20000000u);
        const bool failed_start_async_inactive = failed_start_halted &&
            wait_controller_status(STS_ASYNC, 0u, 20000000u);
        const bool failed_start_periodic_inactive = failed_start_halted &&
            wait_controller_status(STS_PERIODIC, 0u, 20000000u);

        if (!starlix::drivers::ehci::lifecycle::status_confirms_quiescent(
                failed_start_halted, failed_start_async_inactive,
                failed_start_periodic_inactive)) {
            controller_dma_quarantined = true;
            starlix::drivers::serial::print(
                "[EHCI] FATAL: failed start could not prove HALTED with inactive schedules; static DMA objects quarantined until reboot.\n");
        } else {
            starlix::drivers::serial::print(
                "[EHCI] Failed start stopped safely; DMA pages retained.\n");
        }
        op_regs = nullptr;
        cap_regs = nullptr;
        return false;
    }

    controller_active = true;
    controller_was_initialized = true;
    ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::ControllerStarts);
    starlix::drivers::serial::print("[EHCI] Controller state: RUNNING.\n");
    starlix::drivers::serial::print("[EHCI] Host controller running; asynchronous schedule active.\n");

    /* PCI Interrupt Line is legacy routing metadata. Enable it only when it
       names a plausible remapped PIC IRQ; otherwise retain the proven bounded
       polling path. MSI/IOAPIC routing is deliberately outside Phase 7. */
    const uint32_t pci_interrupt_register =
        starlix::drivers::pci::pci_read(bus, slot, function, 0x3Cu);
    const uint8_t irq_line = static_cast<uint8_t>(pci_interrupt_register & 0xFFu);
    if (irq_line >= 3u && irq_line < 16u &&
        starlix::cpu::irq_register_handler(irq_line, ehci_irq_handler)) {
        ehci_irq_line = irq_line;
        ehci_irq_enabled = true;
        clear_status(STS_W1C_MASK);
        dma_full_barrier();
        op_regs->usbintr = EHCI_IRQ_ENABLE_MASK;
        dma_write_barrier();
        starlix::drivers::serial::print("[EHCI] Enabled legacy PIC IRQ ");
        print_dec(ehci_irq_line);
        starlix::drivers::serial::print(" for USBINT/USBERR/PORT_CHANGE/HOST_ERROR/IAA.\n");
    } else {
        ehci_irq_enabled = false;
        ehci_irq_line = 0xFFu;
        op_regs->usbintr = 0;
        starlix::drivers::serial::print("[EHCI] No usable PCI legacy IRQ line; retaining bounded polling.\n");
        starlix::drivers::serial::print("[EHCI] HID periodic completions and root-port changes will be serviced by the PIT-woken kernel loop.\n");
    }

    bool ports_power_settled = true;
    if (hcs_params & HCS_PPC) {
        bool powered_any_port = false;
        for (uint32_t port = 0; port < num_ports; ++port) {
            const uint32_t status = op_regs->portsc[port];
            if ((status & PORT_POWER) == 0) {
                write_port_control(port, (status & (PORT_OWNER | PORT_ENABLE | PORT_SUSPEND)) | PORT_POWER, status & PORT_CHANGE_MASK);
                powered_any_port = true;
            }
        }
        if (powered_any_port && !delay_ms(100)) {
            starlix::drivers::serial::print("[EHCI] WARNING: port power-good delay unavailable; root-port scan skipped for safety.\n");
            ports_power_settled = false;
        }
    }

    if (ports_power_settled) {
        ehci_scan_ports();
        if (!ehci_request_transfer_smoke_test()) {
            starlix::drivers::serial::print("[EHCI][REQ] queued control-transfer execution self-test FAILED; controller remains available for normal callers.\n");
        }
    }
    if (ehci_irq_enabled) {
        starlix::drivers::serial::print("[EHCI] IRQ callbacks observed: ");
        print_dec(ehci_irq_event_latch.interrupt_count());
        starlix::drivers::serial::print(", observed USBSTS causes=0x");
        print_hex16(static_cast<uint16_t>(
            ehci_irq_event_latch.pending_status() & 0xFFFFu));
        starlix::drivers::serial::print(". Synchronous callers still use bounded qTD polling.\n");
    }
    if (ehci_irq_enabled) {
        starlix::drivers::serial::print("[EHCI] IRQ-driven root-port connect/disconnect rescans enabled; hubs and APIC/MSI routing remain future work.\n");
    } else {
        starlix::drivers::serial::print("[EHCI] Hot-plug rescans unavailable without a usable legacy PIC IRQ; hubs and APIC/MSI routing remain future work.\n");
    }
    return true;
}


static bool periodic_budget_lock_acquire() {
    for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&periodic_bandwidth_lock, &expected, 1u,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
        cpu_pause();
    }
    return false;
}

static void periodic_budget_lock_release() {
    __atomic_store_n(&periodic_bandwidth_lock, 0u, __ATOMIC_RELEASE);
}

bool ehci_periodic_bandwidth_reserve(uint8_t device_address, uint8_t endpoint_address,
                                     uint16_t max_packet_size, uint8_t transactions,
                                     uint8_t b_interval, uint32_t* reservation_id) {
    if (!controller_active || !reservation_id || !periodic_budget_lock_acquire()) return false;
    const bool ok = periodic_bandwidth_ledger.reserve(device_address, endpoint_address,
        max_packet_size, transactions, b_interval, reservation_id);
    periodic_budget_lock_release();
    return ok;
}

bool ehci_periodic_bandwidth_release(uint32_t reservation_id) {
    if (!controller_active || !periodic_budget_lock_acquire()) return false;
    const bool ok = periodic_bandwidth_ledger.release(reservation_id);
    periodic_budget_lock_release();
    return ok;
}

bool ehci_periodic_bandwidth_query(uint16_t frame_index, uint8_t microframe,
                                   EHCIPeriodicBandwidthStatus* out) {
    if (!controller_active || !out || !periodic_budget_lock_acquire()) return false;
    uint16_t used = 0u;
    const bool ok = periodic_bandwidth_ledger.usage(frame_index, microframe, &used);
    if (ok) {
        out->frame_index = frame_index;
        out->microframe = microframe;
        out->used_units = used;
        out->budget_units = starlix::drivers::ehci::periodic::MICROFRAME_BUDGET_UNITS;
    }
    periodic_budget_lock_release();
    return ok;
}


static bool periodic_endpoint_lock_acquire() {
    for (uint32_t attempt = 0u; attempt < 1000000u; ++attempt) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&periodic_endpoint_lock, &expected, 1u,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
        cpu_pause();
    }
    return false;
}

static void periodic_endpoint_lock_release() {
    __atomic_store_n(&periodic_endpoint_lock, 0u, __ATOMIC_RELEASE);
}

static EHCIPersistentPeriodicEndpoint* periodic_endpoint_find(uint32_t endpoint_id) {
    if (!endpoint_id) return nullptr;
    for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
        EHCIPersistentPeriodicEndpoint& slot = periodic_endpoints[i];
        if (slot.active && slot.endpoint_id == endpoint_id) return &slot;
    }
    return nullptr;
}

static bool periodic_endpoint_schedule_rebuild() {
    using namespace starlix::drivers::ehci::periodic_schedule;
    if (!controller_active || !op_regs || !periodic_frame_list) return false;

    if (op_regs->usbcmd & CMD_PERIODIC_EN) {
        op_regs->usbcmd &= ~CMD_PERIODIC_EN;
        dma_write_barrier();
        if (!wait_controller_status(STS_PERIODIC, 0u, 20000000u)) {
            starlix::drivers::serial::print(
                "[EHCI][PERIODIC] Could not quiesce live endpoint schedule; halting controller.\n");
            (void)stop_controller();
            return false;
        }
        dma_full_barrier();
    }

    uint16_t periods[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
    bool active[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
    uint32_t qh_addresses[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
    uint8_t order[EHCI_PERIODIC_ENDPOINT_CAPACITY]{};
    uint32_t order_count = 0u;

    for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
        EHCIPersistentPeriodicEndpoint& slot = periodic_endpoints[i];
        active[i] = slot.active;
        periods[i] = slot.period_frames;
        if (!slot.active) continue;
        if (!slot.qh || !slot.qtd || !slot.dma_page ||
            !dma_range32(slot.qh, sizeof(*slot.qh), 32u, qh_addresses[i])) return false;
    }
    if (!build_order(periods, active, EHCI_PERIODIC_ENDPOINT_CAPACITY, order, &order_count))
        return false;

    for (uint32_t position = 0; position < order_count; ++position) {
        const uint8_t index = order[position];
        EHCIPersistentPeriodicEndpoint& slot = periodic_endpoints[index];
        const uint32_t next = (position + 1u < order_count)
            ? (qh_addresses[order[position + 1u]] | LINK_TYPE_QH) : LINK_TERMINATE;
        slot.qh->horizontal_link = next;
    }

    for (uint32_t frame = 0; frame < FRAME_COUNT; ++frame) {
        const uint8_t head = frame_head(static_cast<uint16_t>(frame), periods, order, order_count);
        periodic_frame_list[frame] = (head == NO_NODE)
            ? LINK_TERMINATE : (qh_addresses[head] | LINK_TYPE_QH);
    }

    if (!dma_sync_for_device(periodic_frame_list, EHCI_DMA_PAGE_SIZE)) return false;
    for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
        EHCIPersistentPeriodicEndpoint& slot = periodic_endpoints[i];
        if (!slot.active) continue;
        if (!dma_sync_for_device(slot.qh, sizeof(*slot.qh)) ||
            !dma_sync_for_device(slot.qtd, sizeof(*slot.qtd)) ||
            !dma_sync_for_device(slot.dma_page, EHCI_DMA_PAGE_SIZE)) return false;
    }
    dma_write_barrier();

    if (order_count == 0u) return true;
    op_regs->usbcmd |= CMD_PERIODIC_EN;
    dma_write_barrier();
    if (!wait_controller_status(STS_PERIODIC, STS_PERIODIC, 20000000u)) {
        starlix::drivers::serial::print(
            "[EHCI][PERIODIC] Multi-endpoint schedule failed to start.\n");
        op_regs->usbcmd &= ~CMD_PERIODIC_EN;
        dma_write_barrier();
        if (!wait_controller_status(STS_PERIODIC, 0u, 20000000u)) (void)stop_controller();
        return false;
    }
    return true;
}

static bool periodic_endpoint_rearm(EHCIPersistentPeriodicEndpoint& slot) {
    if (!controller_active || !op_regs || !slot.active || !slot.qh || !slot.qtd || !slot.dma_page)
        return false;
    uint32_t qtd_address = 0u, buffer_address = 0u;
    if (!dma_range32(slot.qtd, sizeof(*slot.qtd), 32u, qtd_address) ||
        !dma_range32(slot.dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, buffer_address))
        return false;

    mem_zero(slot.dma_page, EHCI_DMA_PAGE_SIZE);
    slot.qtd->next_qtd = LINK_TERMINATE;
    slot.qtd->alt_next_qtd = LINK_TERMINATE;
    slot.qtd->token = QTD_ACTIVE | QTD_CERR_3 |
        (static_cast<uint32_t>(QTD_PID_IN) << 8) |
        (static_cast<uint32_t>(slot.max_packet_size) << QTD_BYTES_SHIFT) | QTD_IOC |
        (bulk_data_toggle[slot.device_address][slot.endpoint_address & 0x0Fu][1u] ? QTD_TOGGLE : 0u);
    slot.qtd->buffer[0] = buffer_address;
    for (uint32_t i = 1; i < 5u; ++i) slot.qtd->buffer[i] = 0u;
    slot.qh->current_qtd = 0u;
    slot.qh->overlay.next_qtd = qtd_address;
    slot.qh->overlay.alt_next_qtd = LINK_TERMINATE;
    slot.qh->overlay.token = 0u;
    for (uint32_t i = 0; i < 5u; ++i) slot.qh->overlay.buffer[i] = 0u;
    if (!dma_sync_for_device(slot.dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(slot.qtd, sizeof(*slot.qtd)) ||
        !dma_sync_for_device(slot.qh, sizeof(*slot.qh))) return false;
    dma_write_barrier();
    slot.completion_ready = false;
    slot.completion_error = false;
    slot.completed_bytes = 0u;
    return true;
}

static bool periodic_endpoint_capture_completion(EHCIPersistentPeriodicEndpoint& slot) {
    if (!slot.active || slot.completion_ready || !slot.qtd) return false;
    if (!dma_sync_for_cpu(slot.qtd, sizeof(*slot.qtd))) {
        slot.completion_ready = true;
        slot.completion_error = true;
        slot.completed_bytes = 0u;
        return true;
    }
    dma_read_barrier();
    const uint32_t token = slot.qtd->token;
    if (token & QTD_ACTIVE) return false;

    slot.completion_error = qTD_failed(*slot.qtd);
    uint16_t actual = 0u;
    const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
    if (remaining > slot.max_packet_size) slot.completion_error = true;
    else actual = static_cast<uint16_t>(slot.max_packet_size - remaining);
    slot.completed_bytes = slot.completion_error ? 0u : actual;
    slot.completion_ready = true;
    ehci_diagnostic_add(slot.completion_error
        ? starlix::drivers::ehci::diagnostics::PeriodicErrors
        : starlix::drivers::ehci::diagnostics::PeriodicCompleted);
    if (!slot.completion_error && actual != 0u)
        ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::BytesIn, actual);
    return true;
}

uint32_t ehci_periodic_interrupt_active_count() {
    return __atomic_load_n(&periodic_endpoint_active, __ATOMIC_ACQUIRE);
}

bool ehci_periodic_interrupt_open(uint8_t device_address, uint8_t endpoint_address,
                                  uint16_t max_packet_size, uint8_t b_interval,
                                  uint32_t* endpoint_id) {
    if (!endpoint_id) return false;
    *endpoint_id = 0u;
    if (device_address < 128u && usb_device_uses_uhci[device_address])
        return starlix::drivers::uhci::uhci_periodic_interrupt_open(
            device_address, endpoint_address, max_packet_size, b_interval, endpoint_id);
    uint16_t period_frames = 0u;
    uint8_t start_mask = 0u;
    if (!controller_active || !op_regs ||
        device_address == 0u || device_address > 127u ||
        (endpoint_address & 0x80u) == 0u || (endpoint_address & 0x70u) != 0u ||
        (endpoint_address & 0x0Fu) == 0u || max_packet_size == 0u || max_packet_size > 1024u ||
        !periodic_schedule_parameters(b_interval, &period_frames, &start_mask)) return false;
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return false;
    if (!periodic_endpoint_lock_acquire()) return false;
    if (op_regs->usbcmd & CMD_PERIODIC_EN) {
        /* An already-running schedule can only belong to this persistent manager. */
        if (periodic_endpoint_active == 0u) {
            periodic_endpoint_lock_release();
            return false;
        }
    }

    EHCIPersistentPeriodicEndpoint* slot = nullptr;
    for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
        if (!periodic_endpoints[i].active) { slot = &periodic_endpoints[i]; break; }
    }
    if (!slot) { periodic_endpoint_lock_release(); return false; }

    bool pages_ok = ensure_dma32_page(slot->qh);
    pages_ok = ensure_dma32_page(slot->qtd) && pages_ok;
    pages_ok = ensure_dma32_page(slot->dma_page) && pages_ok;
    if (!pages_ok || !slot->qh || !slot->qtd || !slot->dma_page) {
        periodic_endpoint_lock_release();
        return false;
    }

    uint32_t reservation_id = 0u;
    if (!ehci_periodic_bandwidth_reserve(device_address, endpoint_address,
            max_packet_size, 1u, b_interval, &reservation_id)) {
        periodic_endpoint_lock_release();
        return false;
    }

    uint32_t id = __atomic_fetch_add(&periodic_endpoint_next_id, 1u, __ATOMIC_RELAXED);
    if (id == 0u) id = __atomic_fetch_add(&periodic_endpoint_next_id, 1u, __ATOMIC_RELAXED);
    mem_zero(slot->qh, sizeof(*slot->qh));
    mem_zero(slot->qtd, sizeof(*slot->qtd));
    mem_zero(slot->dma_page, EHCI_DMA_PAGE_SIZE);
    slot->endpoint_id = id;
    slot->reservation_id = reservation_id;
    slot->device_address = device_address;
    slot->endpoint_address = endpoint_address;
    slot->b_interval = b_interval;
    slot->start_mask = start_mask;
    slot->max_packet_size = max_packet_size;
    slot->period_frames = period_frames;
    slot->completion_ready = false;
    slot->completion_error = false;
    slot->completed_bytes = 0u;

    uint32_t qtd_address = 0u, buffer_address = 0u, qh_address = 0u;
    if (!dma_range32(slot->qh, sizeof(*slot->qh), 32u, qh_address) ||
        !dma_range32(slot->qtd, sizeof(*slot->qtd), 32u, qtd_address) ||
        !dma_range32(slot->dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, buffer_address)) {
        slot->endpoint_id = 0u;
        (void)ehci_periodic_bandwidth_release(reservation_id);
        periodic_endpoint_lock_release();
        return false;
    }

    slot->qh->horizontal_link = LINK_TERMINATE;
    slot->qh->ep_characteristics =
        (static_cast<uint32_t>(device_address) & 0x7Fu) |
        (static_cast<uint32_t>(endpoint_address & 0x0Fu) << 8) |
        QH_TOGGLE_CONTROL | QH_SPEED_HIGH |
        (static_cast<uint32_t>(max_packet_size) << QH_MAX_PACKET_SHIFT);
    slot->qh->ep_capabilities = static_cast<uint32_t>(start_mask) | (1u << 30);
    slot->qh->current_qtd = 0u;
    slot->qh->overlay.next_qtd = qtd_address;
    slot->qh->overlay.alt_next_qtd = LINK_TERMINATE;
    slot->qh->overlay.token = 0u;
    for (uint32_t i = 0; i < 5u; ++i) slot->qh->overlay.buffer[i] = 0u;
    slot->qtd->next_qtd = LINK_TERMINATE;
    slot->qtd->alt_next_qtd = LINK_TERMINATE;
    slot->qtd->token = QTD_ACTIVE | QTD_CERR_3 | (static_cast<uint32_t>(QTD_PID_IN) << 8) |
        (static_cast<uint32_t>(max_packet_size) << QTD_BYTES_SHIFT) | QTD_IOC |
        (bulk_data_toggle[device_address][endpoint_address & 0x0Fu][1u] ? QTD_TOGGLE : 0u);
    slot->qtd->buffer[0] = buffer_address;
    for (uint32_t i = 1; i < 5u; ++i) slot->qtd->buffer[i] = 0u;
    slot->active = true;
    __atomic_fetch_add(&periodic_endpoint_active, 1u, __ATOMIC_RELEASE);

    if (!periodic_endpoint_schedule_rebuild()) {
        slot->active = false;
        slot->endpoint_id = 0u;
        __atomic_fetch_sub(&periodic_endpoint_active, 1u, __ATOMIC_RELEASE);
        (void)ehci_periodic_bandwidth_release(reservation_id);
        (void)periodic_endpoint_schedule_rebuild();
        periodic_endpoint_lock_release();
        return false;
    }
    *endpoint_id = id;
    periodic_endpoint_lock_release();
    starlix::drivers::serial::print("[EHCI][PERIODIC] Persistent interrupt-IN endpoint opened.\n");
    return true;
}

EHCIPeriodicPollResult ehci_periodic_interrupt_poll(uint32_t endpoint_id,
                                                    void* out_buffer, uint16_t capacity,
                                                    uint16_t* transferred) {
    if (endpoint_id & 0x80000000u)
        return starlix::drivers::uhci::uhci_periodic_interrupt_poll(
            endpoint_id, out_buffer, capacity, transferred);
    if (transferred) *transferred = 0u;
    if (!periodic_endpoint_lock_acquire()) return EHCIPeriodicPollResult::TransferError;
    EHCIPersistentPeriodicEndpoint* slot = periodic_endpoint_find(endpoint_id);
    if (!slot || !controller_active) {
        periodic_endpoint_lock_release();
        return EHCIPeriodicPollResult::InvalidEndpoint;
    }
    (void)periodic_endpoint_capture_completion(*slot);
    if (!slot->completion_ready) {
        periodic_endpoint_lock_release();
        return EHCIPeriodicPollResult::NoData;
    }

    const bool had_error = slot->completion_error;
    const uint16_t actual = slot->completed_bytes;
    if (!had_error && actual > capacity) {
        periodic_endpoint_lock_release();
        return EHCIPeriodicPollResult::BufferTooSmall;
    }
    if (!had_error && actual != 0u && !out_buffer) {
        periodic_endpoint_lock_release();
        return EHCIPeriodicPollResult::BufferTooSmall;
    }
    if (!had_error && !dma_sync_for_cpu(slot->dma_page, EHCI_DMA_PAGE_SIZE)) {
        slot->completion_ready = true;
        slot->completion_error = true;
        periodic_endpoint_lock_release();
        return EHCIPeriodicPollResult::TransferError;
    }
    if (!had_error && actual) copy_bytes(out_buffer, slot->dma_page, actual);
    const uint32_t final_token = slot->qtd->token;
    if (!had_error) bulk_data_toggle[slot->device_address][slot->endpoint_address & 0x0Fu][1u] =
        (final_token & QTD_TOGGLE) ? 1u : 0u;
    const bool rearmed = periodic_endpoint_rearm(*slot);
    periodic_endpoint_lock_release();
    if (!rearmed || had_error) return EHCIPeriodicPollResult::TransferError;
    if (transferred) *transferred = actual;
    return EHCIPeriodicPollResult::Completed;
}

bool ehci_hid_service() {
    if (!starlix::drivers::ehci::hid_input::service_enabled(
            controller_active, ehci_irq_enabled)) return false;
    bool serviced = false;
    for (uint32_t i = 0u; i < EHCI_HID_BINDING_CAPACITY; ++i) {
        EHCIBoundHIDEndpoint& slot = ehci_hid_endpoints[i];
        if (!slot.active) continue;
        if (slot.device_address == 0u || slot.device_address > 127u ||
            slot.device_generation == 0u ||
            slot.device_generation != usb_device_generation[slot.device_address]) {
            const uint32_t stale_endpoint = slot.endpoint_id;
            if (ehci_periodic_interrupt_close(stale_endpoint))
                ehci_hid_forget_endpoint(stale_endpoint);
            continue;
        }
        uint16_t transferred = 0u;
        const EHCIPeriodicPollResult result = ehci_periodic_interrupt_poll(
            slot.endpoint_id, slot.report, sizeof(slot.report), &transferred);
        if (result == EHCIPeriodicPollResult::NoData) continue;
        serviced = true;
        if (result == EHCIPeriodicPollResult::InvalidEndpoint) {
            ehci_hid_forget_endpoint(slot.endpoint_id);
            continue;
        }
        if (result != EHCIPeriodicPollResult::Completed) {
            if (ehci_hid_transfer_errors != 0xFFFFFFFFu) ++ehci_hid_transfer_errors;
            continue;
        }

        if (slot.keyboard) {
            starlix::drivers::ehci::hid_policy::BootKeyboardReport current{};
            const auto status = starlix::drivers::ehci::hid_policy::decode_boot_keyboard(
                slot.report, transferred, &current);
            if (status == starlix::drivers::ehci::hid_policy::KeyStatus::Rollover)
                continue; /* Preserve the last valid state; no false key-up events. */
            if (status != starlix::drivers::ehci::hid_policy::KeyStatus::Valid) {
                if (ehci_hid_transfer_errors != 0xFFFFFFFFu) ++ehci_hid_transfer_errors;
                continue;
            }
            static uint32_t accepted_keyboard_reports=0u;
            if(accepted_keyboard_reports!=0xFFFFFFFFu)++accepted_keyboard_reports;
            if(accepted_keyboard_reports==1u||(accepted_keyboard_reports&0x7Fu)==0u)
                starlix::drivers::serial::print("[USB][HID] Boot-keyboard report accepted; state delta evaluated.\n");
            if (!slot.have_previous_report) {
                const starlix::drivers::ehci::hid_policy::BootKeyboardReport empty{};
                (void)starlix::drivers::ehci::hid_input::keyboard_report_delta(
                    &empty, &current, &ehci_hid_event_queue);
                slot.have_previous_report = true;
            } else {
                (void)starlix::drivers::ehci::hid_input::keyboard_report_delta(
                    &slot.previous_keyboard, &current, &ehci_hid_event_queue);
            }
            slot.previous_keyboard = current;
        } else {
            starlix::drivers::ehci::hid_policy::BootMouseReport current{};
            bool decoded = false;
            if (slot.generic_mouse) {
                const auto& layout = slot.generic_mouse_layout;
                for (uint8_t report_index = 0u;
                     report_index < layout.report_count && !decoded; ++report_index) {
                    decoded = starlix::drivers::ehci::hid_policy::decode_generic_mouse_report(
                        layout.reports[report_index], slot.report, transferred, &current);
                }
            } else {
                decoded = starlix::drivers::ehci::hid_policy::decode_boot_mouse(
                    slot.report, transferred, &current);
            }
            if (!decoded) {
                if (ehci_hid_transfer_errors != 0xFFFFFFFFu) ++ehci_hid_transfer_errors;
                if ((ehci_hid_transfer_errors & 0x3Fu) == 1u)
                    starlix::drivers::serial::print("[USB][HID] Mouse report rejected (length, report ID or field bounds).\n");
                continue;
            }
            static uint32_t accepted_mouse_reports=0u;
            if(accepted_mouse_reports!=0xFFFFFFFFu)++accepted_mouse_reports;
            if(accepted_mouse_reports==1u||(accepted_mouse_reports&0x7Fu)==0u)
                starlix::drivers::serial::print(slot.generic_mouse
                    ?"[USB][HID] Generic mouse report accepted after layout/length validation.\n"
                    :"[USB][HID] Boot-mouse report accepted after length validation.\n");
            const starlix::drivers::ehci::hid_policy::BootMouseReport previous =
                slot.have_previous_report ? slot.previous_mouse
                                          : starlix::drivers::ehci::hid_policy::BootMouseReport{};
            (void)starlix::drivers::ehci::hid_input::mouse_report_delta(
                &previous, &current, &ehci_hid_event_queue);
            slot.previous_mouse = current;
            slot.have_previous_report = true;
        }
    }
    return serviced;
}

bool ehci_hid_event_poll(EHCIInputEvent* out) {
    return ehci_hid_event_queue.pop(out);
}

uint32_t ehci_hid_active_count() {
    uint32_t active = 0u;
    for (uint32_t i = 0u; i < EHCI_HID_BINDING_CAPACITY; ++i)
        if (ehci_hid_endpoints[i].active) ++active;
    return active;
}

uint32_t ehci_hid_dropped_event_count() {
    return ehci_hid_event_queue.dropped();
}

bool ehci_periodic_interrupt_close(uint32_t endpoint_id) {
    if (endpoint_id & 0x80000000u)
        return starlix::drivers::uhci::uhci_periodic_interrupt_close(endpoint_id);
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return false;
    if (!periodic_endpoint_lock_acquire()) return false;
    EHCIPersistentPeriodicEndpoint* slot = periodic_endpoint_find(endpoint_id);
    if (!slot || !controller_active || !op_regs) {
        periodic_endpoint_lock_release();
        return false;
    }
    const uint32_t reservation_id = slot->reservation_id;
    slot->active = false;
    slot->endpoint_id = 0u;
    slot->completion_ready = false;
    __atomic_fetch_sub(&periodic_endpoint_active, 1u, __ATOMIC_RELEASE);
    const bool schedule_ok = periodic_endpoint_schedule_rebuild();
    if (!schedule_ok) {
        /* A failed close must not report that the endpoint was removed if the
           schedule could not be safely rewritten. Restore the software record
           and attempt to put the old list back; all DMA pages remain allocated. */
        slot->active = true;
        slot->endpoint_id = endpoint_id;
        __atomic_fetch_add(&periodic_endpoint_active, 1u, __ATOMIC_RELEASE);
        if (!controller_dma_quarantined && op_regs) (void)periodic_endpoint_schedule_rebuild();
        periodic_endpoint_lock_release();
        starlix::drivers::serial::print("[EHCI][PERIODIC] Close failed safely; endpoint remains registered.\n");
        return false;
    }
    slot->reservation_id = 0u;
    (void)ehci_periodic_bandwidth_release(reservation_id);
    periodic_endpoint_lock_release();
    return true;
}

bool ehci_service_deferred_work() {
    if (!controller_active) return false;
    bool did_work = false;
    EHCIIRQEvents events{};
    starlix::drivers::ehci::irq::DeferredPlan plan{};
    if (ehci_irq_take_events(&events) && events.pending_status != 0u) {
        plan = starlix::drivers::ehci::irq::classify_deferred_causes(events.pending_status);
        if (plan.has_work()) did_work = true;
        if (plan.record_controller_error) {
            __atomic_fetch_add(&ehci_deferred_error_events, 1u, __ATOMIC_RELAXED);
            starlix::drivers::serial::print("[EHCI][WORK] Deferred controller error cause observed.\n");
        }
        /* Port changes are rescanned by ehci_service_port_changes() after this
           worker pass; async-advance waiters consume the cumulative IAA counter.
           Transfer QH/qTD completion is harvested in this deferred context only
           after a USBINT/USBERRINT/host-error cause was actually latched. */
        if (plan.service_root_port_changes || plan.observe_async_advance)
            did_work = true;
    }
    if (plan.harvest_transfer_completions && periodic_endpoint_lock_acquire()) {
        for (uint32_t i = 0; i < EHCI_PERIODIC_ENDPOINT_CAPACITY; ++i) {
            if (periodic_endpoints[i].active && periodic_endpoint_capture_completion(periodic_endpoints[i]))
                did_work = true;
        }
        periodic_endpoint_lock_release();
    }
    /* Start/reap one nonblocking control/bulk request. Hardware completion is
       observed only after unlink+IAA; PIT-backed monotonic deadlines bound both
       active transfer and IAA reclamation time. IRQ0 wakes the idle HLT between
       deferred service passes, so the worker need not spin continuously. */
    if (ehci_request_service_one()) did_work = true;
    return did_work;
}

bool ehci_deferred_work_pending() {
    if (ehci_irq_event_latch.pending_status() != 0u) return true;
    /* Active requests no longer force a busy loop: the PIT timer wakes HLT and
       lets deferred service check the monotonic request/IAA deadlines. */
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        if (__atomic_load_n(&ehci_request_slots[i].state, __ATOMIC_ACQUIRE) ==
            static_cast<uint32_t>(EHCIRequestState::Queued)) return true;
    }
    /* Periodic qTD completions raise USBINT when a usable legacy PIC IRQ exists.
       The caller may query this with IF cleared; do not acquire a lock which an
       interrupted system-control caller might be holding. */
    return false;
}

EHCIIsochronousResult ehci_isochronous_transfer(uint8_t device_address,
                                                uint8_t endpoint_address,
                                                uint8_t b_interval,
                                                uint16_t max_packet_size,
                                                void* data_buffer, uint16_t data_length,
                                                uint16_t* transferred) {
    using namespace starlix::drivers::ehci::isochronous;
    EHCIIsoDiagnosticScope diagnostic_scope{};
    if (transferred) *transferred = 0u;
    ++isochronous_stats.attempts;
    uint16_t period_frames = 0u;
    if (!valid_request(device_address, endpoint_address, max_packet_size, data_buffer, data_length) ||
        !interval_period_frames(b_interval, &period_frames))
        return EHCIIsochronousResult::InvalidRequest;
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) return EHCIIsochronousResult::Busy;
    if (!controller_active || !op_regs || !periodic_frame_list || periodic_endpoint_active != 0u ||
        (op_regs->usbcmd & CMD_PERIODIC_EN) != 0u) return EHCIIsochronousResult::Busy;
    if (!isochronous_itd_storage || !isochronous_dma_page) return EHCIIsochronousResult::DmaFailure;

    uint32_t itd_address = 0u, buffer_address = 0u;
    if (!dma_range32(isochronous_itd_storage, sizeof(*isochronous_itd_storage), 32u, itd_address) ||
        !dma_range32(isochronous_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, buffer_address))
        return EHCIIsochronousResult::DmaFailure;

    mem_zero(isochronous_itd_storage, sizeof(*isochronous_itd_storage));
    mem_zero(isochronous_dma_page, EHCI_DMA_PAGE_SIZE);
    const bool data_in = (endpoint_address & 0x80u) != 0u;
    if (!data_in) copy_bytes(isochronous_dma_page, data_buffer, data_length);
    isochronous_itd_storage->next_link = LINK_TERMINATE;
    for (uint32_t i = 0; i < 8u; ++i) isochronous_itd_storage->transaction[i] = 0u;
    isochronous_itd_storage->transaction[0] = transaction_token(data_length);
    isochronous_itd_storage->buffer[0] = page0_pointer(buffer_address, device_address, endpoint_address);
    isochronous_itd_storage->buffer[1] = page1_pointer(buffer_address, max_packet_size, data_in);
    isochronous_itd_storage->buffer[2] = page2_pointer(buffer_address, 1u);
    for (uint32_t i = 3u; i < 7u; ++i) isochronous_itd_storage->buffer[i] = buffer_address & PAGE_MASK;

    const uint16_t start_frame = static_cast<uint16_t>((op_regs->frindex >> 3) & FRAME_MASK);
    uint16_t target_frame = 0u;
    uint16_t target_delta = 0u;
    if (!aligned_target_frame(start_frame, period_frames, &target_frame, &target_delta))
        return EHCIIsochronousResult::InvalidRequest;
    for (uint32_t i = 0; i < 1024u; ++i) periodic_frame_list[i] = LINK_TERMINATE;
    periodic_frame_list[target_frame] = itd_address; /* iTD link type is 00b. */
    if (!dma_sync_for_device(isochronous_itd_storage, sizeof(*isochronous_itd_storage)) ||
        !dma_sync_for_device(isochronous_dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_device(periodic_frame_list, EHCI_DMA_PAGE_SIZE))
        return EHCIIsochronousResult::DmaFailure;
    dma_write_barrier();

    op_regs->usbcmd |= CMD_PERIODIC_EN;
    dma_write_barrier();
    if (!wait_controller_status(STS_PERIODIC, STS_PERIODIC, 20000000u)) {
        (void)stop_periodic_schedule();
        return EHCIIsochronousResult::ScheduleFailure;
    }

    bool completed = false;
    bool target_frame_passed = false;
    for (uint32_t spin = 0; spin < 50000000u; ++spin) {
        dma_read_barrier();
        const uint32_t token = isochronous_itd_storage->transaction[0];
        if (!transaction_active(token)) { completed = true; break; }
        const uint16_t current_frame = static_cast<uint16_t>((op_regs->frindex >> 3) & FRAME_MASK);
        const uint16_t delta = static_cast<uint16_t>((current_frame - start_frame) & FRAME_MASK);
        if (delta > target_delta) target_frame_passed = true;
        if (delta > static_cast<uint16_t>(target_delta + 1u)) break;
        cpu_pause();
    }

    if (!stop_periodic_schedule()) return EHCIIsochronousResult::ScheduleFailure;
    if (!dma_sync_for_cpu(isochronous_itd_storage, sizeof(*isochronous_itd_storage)) ||
        !dma_sync_for_cpu(isochronous_dma_page, EHCI_DMA_PAGE_SIZE) ||
        !dma_sync_for_cpu(periodic_frame_list, EHCI_DMA_PAGE_SIZE)) {
        (void)stop_controller();
        return EHCIIsochronousResult::DmaFailure;
    }
    dma_read_barrier();
    const uint32_t token = isochronous_itd_storage->transaction[0];
    if (!completed || transaction_active(token)) {
        if (target_frame_passed) {
            ++isochronous_stats.missed_microframes;
            return EHCIIsochronousResult::MissedMicroframe;
        }
        ++isochronous_stats.timeouts;
        return EHCIIsochronousResult::Timeout;
    }
    if (token & TRANSACTION_BUFFER_ERROR) {
        ++isochronous_stats.buffer_errors;
        return EHCIIsochronousResult::BufferError;
    }
    if (token & TRANSACTION_ERROR_MASK) {
        ++isochronous_stats.transaction_errors;
        return EHCIIsochronousResult::TransactionError;
    }
    uint16_t actual = 0u;
    if (!actual_length(token, data_in, data_length, &actual)) {
        ++isochronous_stats.buffer_errors;
        return EHCIIsochronousResult::BufferError;
    }
    if (data_in && actual) copy_bytes(data_buffer, isochronous_dma_page, actual);
    if (transferred) *transferred = actual;
    ++isochronous_stats.completed;
    diagnostic_scope.succeeded = true;
    return EHCIIsochronousResult::Success;
}

void ehci_isochronous_get_stats(EHCIIsochronousStats* out) {
    if (!out) return;
    *out = isochronous_stats;
}

/* Lifecycle APIs are deliberately serialized and non-IRQ only. Callers must
   quiesce VFS/storage and other USB clients first. DMA pages are retained: this
   allocator has no page-free API and retaining them avoids reuse while a device
   might still have observed an address. Reinitialize by calling ehci_init(). */
bool ehci_diagnostics_snapshot(starlix::drivers::ehci::diagnostics::Snapshot* out) {
    if (!out) return false;
    for (uint32_t i = 0u; i < starlix::drivers::ehci::diagnostics::Count; ++i)
        out->value[i] = __atomic_load_n(&ehci_diagnostic_values[i], __ATOMIC_ACQUIRE);
    return true;
}

EHCIControllerState ehci_controller_state() {
    return static_cast<EHCIControllerState>(
        starlix::drivers::ehci::lifecycle::reported_state_code(
            controller_dma_quarantined, controller_active, controller_was_initialized));
}

bool ehci_controller_shutdown() {
    if (!starlix::drivers::ehci::lifecycle::shutdown_entry_allowed(
            controller_dma_quarantined, op_regs != nullptr, controller_active)) {
        if (controller_dma_quarantined) {
            starlix::drivers::serial::print(
                "[EHCI] Shutdown refused: DMA remains quarantined after a prior halt failure.\n");
        } else {
            controller_dma_quarantined = true;
            starlix::drivers::serial::print(
                "[EHCI] FATAL: controller marked active without operational registers.\n");
        }
        return false;
    }
    if (!op_regs) return true; /* Safe no-op for Uninitialized or already Stopped. */

    /* The transfer engine uses shared QH/qTD and bounce-page storage. Claim it
       before stopping schedules, then reject shutdown if queued request owners
       still exist. This keeps direct synchronous callers from reusing the QHs. */
    EHCITransferEngineGuard transfer_guard;
    if (!transfer_guard.acquired) {
        (void)starlix::drivers::ehci::lifecycle::shutdown_request_owners_clear(
            false, true, true);
        starlix::drivers::serial::print(
            "[EHCI] Shutdown deferred: transfer engine is busy.\n");
        return false;
    }
    const bool request_worker_idle =
        __atomic_load_n(&ehci_request_service_busy, __ATOMIC_ACQUIRE) == 0u;
    bool request_slots_free = true;
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        const uint32_t state = __atomic_load_n(&ehci_request_slots[i].state, __ATOMIC_ACQUIRE);
        if (state != static_cast<uint32_t>(EHCIRequestState::Free)) {
            request_slots_free = false;
            break;
        }
    }
    bool bot_commands_idle = true;
    for (uint32_t address = 1u; address < 128u; ++address) {
        if (__atomic_load_n(&usb_msc_devices[address].bot_lock, __ATOMIC_ACQUIRE) != 0u) {
            bot_commands_idle = false;
            break;
        }
    }
    if (!bot_commands_idle) {
        starlix::drivers::serial::print(
            "[EHCI] Shutdown deferred: an MSC BOT command owns its CBW/data/CSW sequence.\n");
        return false;
    }
    if (!starlix::drivers::ehci::lifecycle::shutdown_request_owners_clear(
            transfer_guard.acquired, request_worker_idle, request_slots_free)) {
        if (!request_worker_idle) {
            starlix::drivers::serial::print(
                "[EHCI] Shutdown deferred: request worker is active.\n");
        } else {
            starlix::drivers::serial::print(
                "[EHCI] Shutdown deferred: release or drain pending request slots first.\n");
        }
        return false;
    }

    const bool periodic_lock_acquired = periodic_budget_lock_acquire();
    if (!periodic_lock_acquired) {
        if (!starlix::drivers::ehci::lifecycle::shutdown_periodic_ledger_clear(
                false, 0u)) {
            starlix::drivers::serial::print("[EHCI] Shutdown deferred: periodic bandwidth ledger is busy.\n");
        }
        return false;
    }
    const uint32_t periodic_reservations = periodic_bandwidth_ledger.active_count();
    periodic_budget_lock_release();
    if (!starlix::drivers::ehci::lifecycle::shutdown_periodic_ledger_clear(
            true, periodic_reservations)) {
        starlix::drivers::serial::print("[EHCI] Shutdown deferred: release periodic bandwidth reservations first.\n");
        return false;
    }

    if (!stop_controller()) return false;
    if (!wait_controller_status(STS_ASYNC | STS_PERIODIC, 0u, 20000000u)) {
        controller_dma_quarantined = true;
        starlix::drivers::serial::print(
            "[EHCI] FATAL: schedules did not become inactive after halt; DMA quarantined.\n");
        return false;
    }

    /* Do not free/reuse controller-visible pages. Stop port routing and clear
       stale status while MMIO is still valid, then discard only CPU MMIO views. */
    op_regs->usbintr = 0u;
    clear_status(STS_W1C_MASK);
    op_regs->configflag = 0u;
    dma_full_barrier();
    op_regs = nullptr;
    cap_regs = nullptr;
    controller_active = false;
    controller_was_initialized = true;
    ehci_irq_enabled = false;
    ehci_irq_line = 0xFFu;
    num_ports = 0u;
    hcs_params = 0u;
    next_device_address = 1u;
    enumerated_port_mask = 0u;
    mem_zero(port_device_address, sizeof(port_device_address));
    mem_zero(usb_device_address_in_use, sizeof(usb_device_address_in_use));
    mem_zero(usb_device_lifecycle_state, sizeof(usb_device_lifecycle_state));
    mem_zero(usb_device_generation, sizeof(usb_device_generation));
    mem_zero(usb_device_model, sizeof(usb_device_model));
    __atomic_store_n(&ehci_port_change_generation, 0u, __ATOMIC_RELAXED);
    ehci_port_change_serviced_generation = 0u;
    mem_zero(device_ep0_max_packet, sizeof(device_ep0_max_packet));
    device_ep0_max_packet[0] = 64u;
    mem_zero(usb_msc_devices, sizeof(usb_msc_devices));
    mem_zero(bulk_data_toggle, sizeof(bulk_data_toggle));
    mem_zero(ehci_hid_endpoints, sizeof(ehci_hid_endpoints));
    ehci_hid_event_queue.clear();
    starlix::drivers::serial::print(
        "[EHCI] Controller stopped safely; DMA pages retained for reinitialization.\n");
    return true;
}

bool ehci_controller_reinitialize() {
    if (!starlix::drivers::ehci::lifecycle::reinitialize_preflight_allowed(
            controller_dma_quarantined)) {
        starlix::drivers::serial::print(
            "[EHCI] Reinitialize refused: DMA quarantine requires reboot.\n");
        return false;
    }
    const bool shutdown_succeeded = ehci_controller_shutdown();
    if (!starlix::drivers::ehci::lifecycle::reinitialize_after_shutdown_allowed(
            controller_dma_quarantined, shutdown_succeeded)) {
        starlix::drivers::serial::print(
            "[EHCI] Reinitialize refused: shutdown did not reach a DMA-safe stopped state.\n");
        return false;
    }
    return ehci_init();
}

static inline uint32_t request_state_load(const EHCIRequestSlot* slot) {
    return __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
}

static EHCIRequestSlot* request_find(uint32_t request_id) {
    if (!request_id) return nullptr;
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        EHCIRequestSlot* slot = &ehci_request_slots[i];
        const uint32_t state = request_state_load(slot);
        if (state == static_cast<uint32_t>(EHCIRequestState::Free) ||
            state == static_cast<uint32_t>(EHCIRequestState::Reserving)) continue;
        if (__atomic_load_n(&slot->request_id, __ATOMIC_ACQUIRE) == request_id)
            return slot;
    }
    return nullptr;
}

static bool request_validate(const EHCITransferRequest* request) {
    if (!request || request->device_address > 127u ||
        !starlix::drivers::ehci::request_policy::buffer_length_valid(
            request->data_length, request->data_buffer != nullptr,
            EHCI_MAX_TRANSFER_BYTES)) return false;
    if (request->type == EHCIRequestType::Control) {
        return request->endpoint_address == 0u &&
               request->setup.wLength == request->data_length;
    }
    if (request->type == EHCIRequestType::Bulk) {
        return request->device_address != 0u &&
               (request->endpoint_address & 0x70u) == 0u &&
               (request->endpoint_address & 0x0Fu) != 0u &&
               request->max_packet_size != 0u &&
               request->max_packet_size <= 512u;
    }
    return false;
}

bool ehci_request_queue_init() {
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&ehci_request_queue_state, &expected, 1u,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
            EHCIRequestSlot* slot = &ehci_request_slots[i];
            __atomic_store_n(&slot->request_id, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&slot->result,
                static_cast<uint32_t>(EHCIRequestResult::Pending), __ATOMIC_RELAXED);
            __atomic_store_n(&slot->transferred, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&slot->cancel_requested, 0u, __ATOMIC_RELAXED);
            mem_zero(&slot->transfer, sizeof(slot->transfer));
            __atomic_store_n(&slot->state, static_cast<uint32_t>(EHCIRequestState::Free), __ATOMIC_RELAXED);
        }
        __atomic_store_n(&ehci_request_next_id, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&ehci_request_queue_state, 2u, __ATOMIC_RELEASE);
        return true;
    }
    if (expected == 2u) return true;
    /* Initialization should be brief. Do not spin forever if interrupted mid-init. */
    uint32_t timeout = 1000000u;
    while (timeout-- && __atomic_load_n(&ehci_request_queue_state, __ATOMIC_ACQUIRE) == 1u)
        cpu_pause();
    return __atomic_load_n(&ehci_request_queue_state, __ATOMIC_ACQUIRE) == 2u;
}

bool ehci_request_submit(const EHCITransferRequest* request, uint32_t* request_id) {
    if (!request_id) return false;
    *request_id = 0u;
    if (!request_validate(request) || !ehci_request_queue_init()) return false;

    EHCIRequestSlot* selected = nullptr;
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        uint32_t expected = static_cast<uint32_t>(EHCIRequestState::Free);
        if (__atomic_compare_exchange_n(&ehci_request_slots[i].state, &expected,
                static_cast<uint32_t>(EHCIRequestState::Reserving), false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            selected = &ehci_request_slots[i];
            break;
        }
    }
    if (!selected) return false;

    uint32_t id = __atomic_fetch_add(&ehci_request_next_id, 1u, __ATOMIC_RELAXED);
    if (id == 0u) id = __atomic_fetch_add(&ehci_request_next_id, 1u, __ATOMIC_RELAXED);
    selected->transfer = *request;
    __atomic_store_n(&selected->request_id, id, __ATOMIC_RELAXED);
    __atomic_store_n(&selected->result, static_cast<uint32_t>(EHCIRequestResult::Pending), __ATOMIC_RELAXED);
    __atomic_store_n(&selected->transferred, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&selected->cancel_requested, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&selected->state, static_cast<uint32_t>(EHCIRequestState::Queued), __ATOMIC_RELEASE);
    *request_id = id;
    ehci_diagnostic_add(starlix::drivers::ehci::diagnostics::AsyncSubmitted);
    return true;
}

bool ehci_request_dequeue(EHCIQueuedRequest* out) {
    if (!out || !ehci_request_queue_init()) return false;
    for (uint32_t attempt = 0; attempt < EHCI_REQUEST_CAPACITY; ++attempt) {
        EHCIRequestSlot* candidate = nullptr;
        uint32_t candidate_id = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
            EHCIRequestSlot* slot = &ehci_request_slots[i];
            if (request_state_load(slot) != static_cast<uint32_t>(EHCIRequestState::Queued)) continue;
            const uint32_t id = __atomic_load_n(&slot->request_id, __ATOMIC_ACQUIRE);
            if (id && id < candidate_id) { candidate = slot; candidate_id = id; }
        }
        if (!candidate) return false;
        uint32_t expected = static_cast<uint32_t>(EHCIRequestState::Queued);
        if (!__atomic_compare_exchange_n(&candidate->state, &expected,
                static_cast<uint32_t>(EHCIRequestState::Active), false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        out->request_id = __atomic_load_n(&candidate->request_id, __ATOMIC_ACQUIRE);
        out->transfer = candidate->transfer;
        out->state = EHCIRequestState::Active;
        return true;
    }
    return false;
}

/*
 * Start or advance one queued control/bulk request in non-IRQ context. Submit
 * does not wait for hardware completion: the worker publishes a QH, then later
 * observes qTD outcome and requests unlink/IAA before DMA ownership is returned.
 * The deferred worker uses PIT-backed monotonic deadlines. The public wait helper
 * may sleep with STI;HLT until IRQ0/USB IRQ wakes it; there is no scheduler-backed
 * wait queue yet, so callers must not invoke it from IRQ context or with IF clear.
 */
static EHCIRequestSlot* ehci_async_current_slot() {
    const uint32_t id = __atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE);
    return request_find(id);
}

/* Result inspection is intentionally side-effect-free. DMA ownership is not
   handed back to the CPU until the QH has been unlinked and IAA observed. */
static uint32_t ehci_async_transfer_outcome() {
    if (ehci_async_execution.request_type == static_cast<uint8_t>(EHCIRequestType::Control)) {
        const uint32_t setup_token = setup_qtd_storage->token;
        const uint32_t status_token = status_qtd_storage->token;
        const bool setup_done = (setup_token & QTD_ACTIVE) == 0u;
        const bool status_done = (status_token & QTD_ACTIVE) == 0u;
        if ((setup_done && qTD_failed(*setup_qtd_storage)) ||
            (status_done && qTD_failed(*status_qtd_storage)))
            return starlix::drivers::ehci::async_request_policy::POLL_ERROR;

        bool data_done = true;
        bool short_in_packet = false;
        for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i) {
            const uint32_t token = data_qtd_storage[i].token;
            if ((token & QTD_ACTIVE) == 0u) {
                if (qTD_failed(data_qtd_storage[i]))
                    return starlix::drivers::ehci::async_request_policy::POLL_ERROR;
                const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
                const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
                const uint32_t chunk = (ehci_async_execution.requested_bytes - offset) > EHCI_DMA_PAGE_SIZE
                    ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
                    : ehci_async_execution.requested_bytes - offset;
                if (remaining > chunk)
                    return starlix::drivers::ehci::async_request_policy::POLL_ERROR;
                if (ehci_async_execution.data_in && remaining != 0u) short_in_packet = true;
            } else if (!short_in_packet) {
                data_done = false;
            }
        }
        if (setup_done && status_done &&
            (data_done || (ehci_async_execution.data_in && short_in_packet)))
            return starlix::drivers::ehci::async_request_policy::POLL_SUCCESS;
        return starlix::drivers::ehci::async_request_policy::POLL_PENDING;
    }

    if (ehci_async_execution.request_type == static_cast<uint8_t>(EHCIRequestType::Bulk)) {
        bool all_done = true;
        bool short_in_packet = false;
        for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i) {
            const uint32_t token = bulk_qtd_storage[i].token;
            if (token & QTD_ACTIVE) {
                if (!short_in_packet) all_done = false;
                continue;
            }
            if (qTD_failed(bulk_qtd_storage[i]))
                return starlix::drivers::ehci::async_request_policy::POLL_ERROR;
            const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint32_t chunk = ehci_async_execution.requested_bytes
                ? ((ehci_async_execution.requested_bytes - offset) > EHCI_DMA_PAGE_SIZE
                    ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
                    : ehci_async_execution.requested_bytes - offset) : 0u;
            if (remaining > chunk)
                return starlix::drivers::ehci::async_request_policy::POLL_ERROR;
            if (ehci_async_execution.data_in && remaining != 0u) short_in_packet = true;
        }
        return (all_done || short_in_packet)
            ? starlix::drivers::ehci::async_request_policy::POLL_SUCCESS
            : starlix::drivers::ehci::async_request_policy::POLL_PENDING;
    }
    return starlix::drivers::ehci::async_request_policy::POLL_ERROR;
}

static bool ehci_async_start_hardware(const EHCIQueuedRequest& queued) {
    if (!controller_active || !op_regs || !async_head_qh || !async_request_qh ||
        !starlix::cpu::timer_is_ready() ||
        (op_regs->usbcmd & CMD_ASYNC_EN) == 0u) return false;
    EHCIRequestSlot* slot = request_find(queued.request_id);
    if (!slot || request_state_load(slot) != static_cast<uint32_t>(EHCIRequestState::Active))
        return false;

    uint32_t head_address = 0u, request_qh_address = 0u;
    if (!dma_range32(async_head_qh, sizeof(*async_head_qh), 32u, head_address) ||
        !dma_range32(async_request_qh, sizeof(*async_request_qh), 32u, request_qh_address))
        return false;

    const EHCITransferRequest& request = queued.transfer;
    uint32_t qtd_count = 0u;
    uint32_t qtd_base = 0u;
    uint32_t setup_qtd_address = 0u, status_qtd_address = 0u, setup_buffer_address = 0u;
    uint32_t buffer_addresses[EHCI_MAX_TRANSFER_PAGES]{};
    bool data_in = false;

    if (request.type == EHCIRequestType::Control) {
        if (request.device_address > 127u || request.setup.wLength != request.data_length ||
            (request.data_length != 0u && !request.data_buffer) ||
            request.data_length > EHCI_MAX_TRANSFER_BYTES ||
            device_ep0_max_packet[request.device_address] == 0u ||
            !dma_range32(setup_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, setup_qtd_address) ||
            !dma_range32(data_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, qtd_base) ||
            !dma_range32(status_qtd_storage, sizeof(*status_qtd_storage), 32u, status_qtd_address) ||
            !dma_range32(setup_dma_page, EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE, setup_buffer_address))
            return false;
        qtd_count = request.data_length
            ? static_cast<uint32_t>((request.data_length + EHCI_DMA_PAGE_SIZE - 1u) / EHCI_DMA_PAGE_SIZE)
            : 0u;
        for (uint32_t i = 0u; i < qtd_count; ++i) {
            if (!dma_range32(data_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE,
                             buffer_addresses[i])) return false;
        }
        mem_zero(setup_dma_page, EHCI_DMA_PAGE_SIZE);
        zero_dma_pages(data_dma_pages);
        copy_bytes(setup_dma_page, &request.setup, sizeof(USB_SETUP_PACKET));
        data_in = (request.setup.bmRequestType & 0x80u) != 0u;
        if (request.data_length && !data_in)
            copy_to_dma_pages(data_dma_pages, request.data_buffer, request.data_length);

        const uint32_t first_after_setup = qtd_count ? qtd_base : status_qtd_address;
        prepare_qtd(*setup_qtd_storage, first_after_setup, LINK_TERMINATE,
                    QTD_PID_SETUP, sizeof(USB_SETUP_PACKET), false, setup_buffer_address);
        setup_qtd_storage->token |= QTD_IOC;
        const uint32_t data_pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
        for (uint32_t i = 0u; i < qtd_count; ++i) {
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint16_t chunk = static_cast<uint16_t>(
                (request.data_length - offset) > EHCI_DMA_PAGE_SIZE
                    ? EHCI_DMA_PAGE_SIZE : request.data_length - offset);
            const uint32_t next = (i + 1u < qtd_count)
                ? qtd_base + ((i + 1u) * sizeof(EHCI_QTD)) : status_qtd_address;
            prepare_qtd(data_qtd_storage[i], next,
                        data_in ? status_qtd_address : LINK_TERMINATE,
                        data_pid, chunk, true, buffer_addresses[i]);
            data_qtd_storage[i].token |= QTD_IOC;
        }
        const uint32_t status_pid = (request.data_length && data_in) ? QTD_PID_OUT : QTD_PID_IN;
        prepare_qtd(*status_qtd_storage, LINK_TERMINATE, LINK_TERMINATE,
                    status_pid, 0u, true, 0u);
        status_qtd_storage->token |= QTD_IOC;

        bool ready = dma_sync_for_device(setup_dma_page, EHCI_DMA_PAGE_SIZE) &&
                     dma_sync_for_device(setup_qtd_storage, sizeof(*setup_qtd_storage)) &&
                     dma_sync_for_device(status_qtd_storage, sizeof(*status_qtd_storage));
        if (qtd_count) {
            ready = dma_sync_for_device(data_qtd_storage,
                static_cast<size_t>(qtd_count) * sizeof(EHCI_QTD)) && ready;
            for (uint32_t i = 0u; i < qtd_count; ++i)
                ready = dma_sync_for_device(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) && ready;
        }
        if (!ready) return false;
        /* Control endpoints keep DATA toggle in the QH overlay; DTC must stay
           clear so multi-packet control data stages alternate DATA1/DATA0. */
        async_request_qh->ep_characteristics =
            (static_cast<uint32_t>(request.device_address) & 0x7Fu) |
            QH_SPEED_HIGH |
            (static_cast<uint32_t>(device_ep0_max_packet[request.device_address]) << QH_MAX_PACKET_SHIFT);
        async_request_qh->overlay.next_qtd = setup_qtd_address;
    } else if (request.type == EHCIRequestType::Bulk) {
        if (request.device_address == 0u || request.device_address > 127u ||
            (request.endpoint_address & 0x70u) != 0u ||
            (request.endpoint_address & 0x0Fu) == 0u ||
            request.max_packet_size == 0u || request.max_packet_size > 512u ||
            request.data_length > EHCI_MAX_TRANSFER_BYTES ||
            (request.data_length != 0u && !request.data_buffer) ||
            !dma_range32(bulk_qtd_storage, EHCI_DMA_PAGE_SIZE, 32u, qtd_base)) return false;
        qtd_count = request.data_length
            ? static_cast<uint32_t>((request.data_length + EHCI_DMA_PAGE_SIZE - 1u) / EHCI_DMA_PAGE_SIZE)
            : 1u;
        for (uint32_t i = 0u; i < EHCI_MAX_TRANSFER_PAGES; ++i) {
            if (!dma_range32(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE, EHCI_DMA_PAGE_SIZE,
                             buffer_addresses[i])) return false;
        }
        zero_dma_pages(bulk_dma_pages);
        data_in = (request.endpoint_address & 0x80u) != 0u;
        if (request.data_length && !data_in)
            copy_to_dma_pages(bulk_dma_pages, request.data_buffer, request.data_length);
        const uint32_t pid = data_in ? QTD_PID_IN : QTD_PID_OUT;
        for (uint32_t i = 0u; i < qtd_count; ++i) {
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint16_t chunk = request.data_length
                ? static_cast<uint16_t>((request.data_length - offset) > EHCI_DMA_PAGE_SIZE
                    ? EHCI_DMA_PAGE_SIZE : request.data_length - offset) : 0u;
            const uint32_t next = (i + 1u < qtd_count)
                ? qtd_base + ((i + 1u) * sizeof(EHCI_QTD)) : LINK_TERMINATE;
            prepare_qtd(bulk_qtd_storage[i], next, LINK_TERMINATE,
                        pid, chunk, false, chunk ? buffer_addresses[i] : 0u);
            bulk_qtd_storage[i].token |= QTD_IOC;
        }
        bool ready = dma_sync_for_device(bulk_qtd_storage,
            static_cast<size_t>(qtd_count) * sizeof(EHCI_QTD));
        for (uint32_t i = 0u; i < qtd_count; ++i)
            ready = dma_sync_for_device(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE) && ready;
        if (!ready) return false;
        const uint8_t endpoint = request.endpoint_address & 0x0Fu;
        const uint8_t direction = data_in ? 1u : 0u;
        async_request_qh->ep_characteristics =
            (static_cast<uint32_t>(request.device_address) & 0x7Fu) |
            (static_cast<uint32_t>(endpoint) << 8) | QH_SPEED_HIGH |
            (static_cast<uint32_t>(request.max_packet_size) << QH_MAX_PACKET_SHIFT);
        async_request_qh->overlay.next_qtd = qtd_base;
        async_request_qh->overlay.token = bulk_data_toggle[request.device_address][endpoint][direction]
            ? QTD_TOGGLE : 0u;
    } else {
        return false;
    }

    async_request_qh->horizontal_link = head_address | LINK_TYPE_QH;
    async_request_qh->ep_capabilities = (1u << 30);
    async_request_qh->current_qtd = 0u;
    async_request_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    if (request.type == EHCIRequestType::Control) async_request_qh->overlay.token = 0u;
    for (uint32_t i = 0u; i < 5u; ++i) async_request_qh->overlay.buffer[i] = 0u;
    async_head_qh->ep_characteristics = make_qh_characteristics(0u, 64u);
    async_head_qh->ep_capabilities = (1u << 30);
    async_head_qh->current_qtd = 0u;
    async_head_qh->overlay.next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.alt_next_qtd = LINK_TERMINATE;
    async_head_qh->overlay.token = 0u;
    for (uint32_t i = 0u; i < 5u; ++i) async_head_qh->overlay.buffer[i] = 0u;
    if (!dma_sync_for_device(async_request_qh, sizeof(*async_request_qh)) ||
        !dma_sync_for_device(async_head_qh, sizeof(*async_head_qh))) return false;
    dma_write_barrier();

    /* Publish last: anchor -> request QH -> anchor. The request QH is not the
       reclamation head, and it is not reused until removal plus IAA. */
    async_head_qh->horizontal_link = request_qh_address | LINK_TYPE_QH;
    if (!dma_sync_for_device(async_head_qh, sizeof(*async_head_qh))) {
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();
    __atomic_store_n(&ehci_async_execution.request_id, queued.request_id, __ATOMIC_RELAXED);
    ehci_async_execution.qtd_count = qtd_count;
    ehci_async_execution.iaa_baseline = 0u;
    ehci_async_execution.terminal_result = static_cast<uint32_t>(EHCIRequestResult::Pending);
    ehci_async_execution.request_deadline_ms =
        starlix::drivers::ehci::async_request_policy::deadline_after(
            starlix::cpu::timer_now_ms(), EHCI_ASYNC_REQUEST_TIMEOUT_MS);
    ehci_async_execution.started_ms = starlix::cpu::timer_now_ms();
    ehci_async_execution.iaa_deadline_ms = 0u;
    ehci_async_execution.requested_bytes = request.data_length;
    ehci_async_execution.request_type = static_cast<uint8_t>(request.type);
    ehci_async_execution.data_in = data_in ? 1u : 0u;
    __atomic_store_n(&ehci_async_execution.stage,
        static_cast<uint32_t>(EHCIAsyncExecutionStage::InFlight), __ATOMIC_RELEASE);
    return true;
}

static bool ehci_async_begin_unlink(uint32_t terminal_result) {
    if (!controller_active || !op_regs || !async_head_qh) return false;
    uint32_t head_address = 0u;
    if (!dma_range32(async_head_qh, sizeof(*async_head_qh), 32u, head_address)) return false;
    clear_status(STS_ASYNC_ADVANCE);
    ehci_async_execution.iaa_baseline = ehci_irq_event_latch.auxiliary_count();
    ehci_async_execution.terminal_result = terminal_result;
    async_head_qh->horizontal_link = head_address | LINK_TYPE_QH;
    if (!dma_sync_for_device(async_head_qh, sizeof(*async_head_qh))) {
        (void)stop_controller();
        return false;
    }
    dma_write_barrier();
    op_regs->usbcmd |= CMD_ASYNC_ADVANCE;
    dma_write_barrier();
    ehci_async_execution.iaa_deadline_ms =
        starlix::drivers::ehci::async_request_policy::deadline_after(
            starlix::cpu::timer_now_ms(), EHCI_ASYNC_IAA_TIMEOUT_MS);
    __atomic_store_n(&ehci_async_execution.stage,
        static_cast<uint32_t>(EHCIAsyncExecutionStage::WaitingIaa), __ATOMIC_RELEASE);
    return true;
}

static bool ehci_async_finish_request(uint32_t request_id, EHCIRequestResult result,
                                      uint16_t transferred) {
    EHCIRequestSlot* slot = request_find(request_id);
    if (!slot) return false;
    const uint32_t requested = slot->transfer.data_length;
    if (transferred > requested) { result = EHCIRequestResult::TransferError; transferred = 0u; }
    uint32_t expected = static_cast<uint32_t>(EHCIRequestState::Active);
    if (!__atomic_compare_exchange_n(&slot->state, &expected,
            static_cast<uint32_t>(EHCIRequestState::Completing), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&slot->result, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
    __atomic_store_n(&slot->transferred, transferred, __ATOMIC_RELAXED);
    uint32_t final_state = static_cast<uint32_t>(EHCIRequestState::Failed);
    if (result == EHCIRequestResult::Success) final_state = static_cast<uint32_t>(EHCIRequestState::Completed);
    else if (result == EHCIRequestResult::Cancelled) final_state = static_cast<uint32_t>(EHCIRequestState::Cancelled);
    __atomic_store_n(&slot->state, final_state, __ATOMIC_RELEASE);
    using namespace starlix::drivers::ehci::diagnostics;
    if (result == EHCIRequestResult::Success) {
        ehci_diagnostic_add(AsyncCompleted);
        if (transferred != 0u) {
            const bool data_in = slot->transfer.type == EHCIRequestType::Control
                ? ((slot->transfer.setup.bmRequestType & 0x80u) != 0u)
                : ((slot->transfer.endpoint_address & 0x80u) != 0u);
            ehci_diagnostic_add(data_in ? BytesIn : BytesOut, transferred);
        }
    } else {
        ehci_diagnostic_add(AsyncFailed);
        if (result == EHCIRequestResult::TimedOut) ehci_diagnostic_add(AsyncTimeouts);
        if (result == EHCIRequestResult::Cancelled) ehci_diagnostic_add(AsyncCancelled);
    }
    if (starlix::cpu::timer_is_ready()) {
        const uint64_t end_ms = starlix::cpu::timer_now_ms();
        if (end_ms >= ehci_async_execution.started_ms)
            ehci_diagnostic_max(MaxAsyncLatencyMs, end_ms - ehci_async_execution.started_ms);
    }
    return true;
}

static bool ehci_async_copy_completed_data(uint16_t* actual_out) {
    if (actual_out) *actual_out = 0u;
    bool observed = dma_sync_for_cpu(async_request_qh, sizeof(*async_request_qh)) &&
                   dma_sync_for_cpu(async_head_qh, sizeof(*async_head_qh));
    if (ehci_async_execution.request_type == static_cast<uint8_t>(EHCIRequestType::Control)) {
        observed = dma_sync_for_cpu(setup_qtd_storage, sizeof(*setup_qtd_storage)) && observed;
        observed = dma_sync_for_cpu(status_qtd_storage, sizeof(*status_qtd_storage)) && observed;
        if (ehci_async_execution.qtd_count) {
            observed = dma_sync_for_cpu(data_qtd_storage,
                static_cast<size_t>(ehci_async_execution.qtd_count) * sizeof(EHCI_QTD)) && observed;
            for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i)
                observed = dma_sync_for_cpu(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) && observed;
        }
    } else {
        observed = dma_sync_for_cpu(bulk_qtd_storage,
            static_cast<size_t>(ehci_async_execution.qtd_count) * sizeof(EHCI_QTD)) && observed;
        for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i)
            observed = dma_sync_for_cpu(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE) && observed;
    }
    if (!observed) return false;
    dma_read_barrier();

    const uint32_t outcome = ehci_async_transfer_outcome();
    if (outcome != starlix::drivers::ehci::async_request_policy::POLL_SUCCESS) return false;
    EHCIRequestSlot* slot = ehci_async_current_slot();
    if (!slot) return false;
    uint32_t actual32 = 0u;
    if (ehci_async_execution.request_type == static_cast<uint8_t>(EHCIRequestType::Control)) {
        for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i) {
            const uint32_t token = data_qtd_storage[i].token;
            if (token & QTD_ACTIVE) break;
            const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint32_t chunk = (ehci_async_execution.requested_bytes - offset) > EHCI_DMA_PAGE_SIZE
                ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
                : ehci_async_execution.requested_bytes - offset;
            if (remaining > chunk) return false;
            actual32 += chunk - remaining;
            if (ehci_async_execution.data_in && remaining != 0u) break;
        }
        if (ehci_async_execution.data_in && actual32)
            copy_from_dma_pages(slot->transfer.data_buffer, data_dma_pages, actual32);
    } else {
        for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i) {
            const uint32_t token = bulk_qtd_storage[i].token;
            if (token & QTD_ACTIVE) break;
            const uint32_t remaining = (token & QTD_BYTES_MASK) >> QTD_BYTES_SHIFT;
            const uint32_t offset = i * static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE);
            const uint32_t chunk = ehci_async_execution.requested_bytes
                ? ((ehci_async_execution.requested_bytes - offset) > EHCI_DMA_PAGE_SIZE
                    ? static_cast<uint32_t>(EHCI_DMA_PAGE_SIZE)
                    : ehci_async_execution.requested_bytes - offset) : 0u;
            if (remaining > chunk) return false;
            actual32 += chunk - remaining;
            if (ehci_async_execution.data_in && remaining != 0u) break;
        }
        if (ehci_async_execution.data_in && actual32)
            copy_from_dma_pages(slot->transfer.data_buffer, bulk_dma_pages, actual32);
        const uint8_t endpoint = slot->transfer.endpoint_address & 0x0Fu;
        const uint8_t direction = ehci_async_execution.data_in ? 1u : 0u;
        bulk_data_toggle[slot->transfer.device_address][endpoint][direction] =
            (async_request_qh->overlay.token & QTD_TOGGLE) ? 1u : 0u;
    }
    if (actual32 > 0xFFFFu) return false;
    if (actual_out) *actual_out = static_cast<uint16_t>(actual32);
    return true;
}

static bool ehci_async_service_active() {
    const uint32_t stage = __atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE);
    if (stage == static_cast<uint32_t>(EHCIAsyncExecutionStage::InFlight)) {
        EHCIRequestSlot* slot = ehci_async_current_slot();
        if (!slot) {
            (void)ehci_async_begin_unlink(static_cast<uint32_t>(EHCIRequestResult::TransferError));
            return true;
        }
        const bool cancel = __atomic_load_n(&slot->cancel_requested, __ATOMIC_ACQUIRE) != 0u;
        const bool timeout = starlix::drivers::ehci::async_request_policy::deadline_reached(
            starlix::cpu::timer_now_ms(), ehci_async_execution.request_deadline_ms);
        const uint32_t outcome = ehci_async_transfer_outcome();
        if (starlix::drivers::ehci::async_request_policy::should_detach(
                stage, outcome, cancel, timeout)) {
            uint32_t result = starlix::drivers::ehci::async_request_policy::result_for_unlink(
                outcome, cancel, timeout);
            if (!ehci_async_begin_unlink(result)) {
                (void)stop_controller();
                EHCIRequestResult failed_result = EHCIRequestResult::TransferError;
                (void)ehci_async_finish_request(__atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE), failed_result, 0u);
                __atomic_store_n(&ehci_async_execution.stage,
                    static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle), __ATOMIC_RELEASE);
                __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
            }
            return true;
        }
        return false;
    }

    if (stage != static_cast<uint32_t>(EHCIAsyncExecutionStage::WaitingIaa)) return false;
    const bool iaa_status = op_regs && ((op_regs->usbsts & STS_ASYNC_ADVANCE) != 0u);
    const bool iaa_seen = starlix::drivers::ehci::async_request_policy::iaa_safe_to_finish(
        iaa_status, ehci_irq_event_latch.auxiliary_count(), ehci_async_execution.iaa_baseline);
    bool safe_by_halt = false;
    if (!iaa_seen && starlix::drivers::ehci::async_request_policy::deadline_reached(
            starlix::cpu::timer_now_ms(), ehci_async_execution.iaa_deadline_ms)) {
        safe_by_halt = stop_controller();
        if (!safe_by_halt) {
            (void)ehci_async_finish_request(__atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE),
                EHCIRequestResult::TransferError, 0u);
            __atomic_store_n(&ehci_async_execution.stage,
                static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle), __ATOMIC_RELEASE);
            __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
            return true;
        }
    }
    if (!iaa_seen && !safe_by_halt) return false;
    if (iaa_status) clear_status(STS_ASYNC_ADVANCE);
    dma_full_barrier();

    EHCIRequestResult result = static_cast<EHCIRequestResult>(ehci_async_execution.terminal_result);
    uint16_t transferred = 0u;
    if (result == EHCIRequestResult::Success) {
        if (!ehci_async_copy_completed_data(&transferred)) {
            result = EHCIRequestResult::TransferError;
            transferred = 0u;
        }
    } else if (safe_by_halt) {
        result = EHCIRequestResult::TransferError;
    } else {
        bool sync_ok = dma_sync_for_cpu(async_request_qh, sizeof(*async_request_qh)) &&
                       dma_sync_for_cpu(async_head_qh, sizeof(*async_head_qh));
        if (ehci_async_execution.request_type == static_cast<uint8_t>(EHCIRequestType::Control)) {
            sync_ok = dma_sync_for_cpu(setup_qtd_storage, sizeof(*setup_qtd_storage)) && sync_ok;
            sync_ok = dma_sync_for_cpu(status_qtd_storage, sizeof(*status_qtd_storage)) && sync_ok;
            if (ehci_async_execution.qtd_count) {
                sync_ok = dma_sync_for_cpu(data_qtd_storage,
                    static_cast<size_t>(ehci_async_execution.qtd_count) * sizeof(EHCI_QTD)) && sync_ok;
                for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i)
                    sync_ok = dma_sync_for_cpu(data_dma_pages[i], EHCI_DMA_PAGE_SIZE) && sync_ok;
            }
        } else {
            sync_ok = dma_sync_for_cpu(bulk_qtd_storage,
                static_cast<size_t>(ehci_async_execution.qtd_count) * sizeof(EHCI_QTD)) && sync_ok;
            for (uint32_t i = 0u; i < ehci_async_execution.qtd_count; ++i)
                sync_ok = dma_sync_for_cpu(bulk_dma_pages[i], EHCI_DMA_PAGE_SIZE) && sync_ok;
        }
        if (!sync_ok) result = EHCIRequestResult::TransferError;
    }
    (void)ehci_async_finish_request(__atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE), result, transferred);
    async_request_qh->horizontal_link = LINK_TERMINATE;
    __atomic_store_n(&ehci_async_execution.request_id, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&ehci_async_execution.stage,
        static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle), __ATOMIC_RELEASE);
    __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
    return true;
}

static bool ehci_async_start_failure(uint32_t request_id) {
    (void)ehci_async_finish_request(request_id, EHCIRequestResult::TransferError, 0u);
    __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
    return false;
}

bool ehci_request_service_one() {
    if (!controller_active) return false;
    uint32_t expected_busy = 0u;
    if (!__atomic_compare_exchange_n(&ehci_request_service_busy, &expected_busy, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;

    const uint32_t stage = __atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE);
    if (stage != static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle)) {
        const bool worked = ehci_async_service_active();
        __atomic_store_n(&ehci_request_service_busy, 0u, __ATOMIC_RELEASE);
        return worked;
    }

    /* Reserve shared qTD/bounce-buffer ownership before claiming a queued slot.
       The lock remains held across hardware ownership and the IAA safe point. */
    uint32_t expected_engine = 0u;
    if (!__atomic_compare_exchange_n(&ehci_transfer_engine_busy, &expected_engine, 1u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&ehci_request_service_busy, 0u, __ATOMIC_RELEASE);
        return false;
    }
    EHCIQueuedRequest queued{};
    if (!ehci_request_dequeue(&queued)) {
        __atomic_store_n(&ehci_transfer_engine_busy, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&ehci_request_service_busy, 0u, __ATOMIC_RELEASE);
        return false;
    }
    if (!ehci_async_start_hardware(queued)) {
        const bool done = ehci_async_start_failure(queued.request_id);
        __atomic_store_n(&ehci_request_service_busy, 0u, __ATOMIC_RELEASE);
        return done;
    }
    __atomic_store_n(&ehci_request_service_busy, 0u, __ATOMIC_RELEASE);
    return true;
}

bool ehci_request_complete(uint32_t request_id, EHCIRequestResult result,
                           uint16_t transferred) {
    if (__atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE) == request_id &&
        __atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE) !=
            static_cast<uint32_t>(EHCIAsyncExecutionStage::Idle)) return false;
    if (!starlix::drivers::ehci::request_policy::terminal_result_allowed(
            static_cast<uint32_t>(result))) return false;
    EHCIRequestSlot* slot = request_find(request_id);
    if (!slot) return false;
    const uint32_t requested_length = slot->transfer.data_length;
    if (!starlix::drivers::ehci::request_policy::completion_length_valid(
            requested_length, transferred)) return false;
    uint32_t expected = static_cast<uint32_t>(EHCIRequestState::Active);
    if (!__atomic_compare_exchange_n(&slot->state, &expected,
            static_cast<uint32_t>(EHCIRequestState::Completing), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&slot->result, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
    __atomic_store_n(&slot->transferred, static_cast<uint32_t>(transferred), __ATOMIC_RELAXED);
    const uint32_t terminal =
        starlix::drivers::ehci::request_policy::completion_state_for_result(
            static_cast<uint32_t>(result));
    __atomic_store_n(&slot->state, terminal, __ATOMIC_RELEASE);
    return true;
}

bool ehci_request_cancel(uint32_t request_id) {
    EHCIRequestSlot* slot = request_find(request_id);
    if (!slot) return false;
    const uint32_t state = request_state_load(slot);
    if (state == static_cast<uint32_t>(EHCIRequestState::Active) &&
        __atomic_load_n(&ehci_async_execution.request_id, __ATOMIC_ACQUIRE) == request_id &&
        __atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE) ==
            static_cast<uint32_t>(EHCIAsyncExecutionStage::InFlight)) {
        __atomic_store_n(&slot->cancel_requested, 1u, __ATOMIC_RELEASE);
        if (__atomic_load_n(&ehci_async_execution.stage, __ATOMIC_ACQUIRE) ==
            static_cast<uint32_t>(EHCIAsyncExecutionStage::InFlight))
            return true; /* Deferred worker must unlink the QH and observe IAA. */
        __atomic_store_n(&slot->cancel_requested, 0u, __ATOMIC_RELEASE);
        return false;
    }
    if (!starlix::drivers::ehci::request_policy::cancellation_allowed(state)) return false;
    uint32_t expected = static_cast<uint32_t>(EHCIRequestState::Queued);
    if (!__atomic_compare_exchange_n(&slot->state, &expected,
            static_cast<uint32_t>(EHCIRequestState::Completing), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&slot->result, static_cast<uint32_t>(EHCIRequestResult::Cancelled), __ATOMIC_RELAXED);
    __atomic_store_n(&slot->transferred, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->state, static_cast<uint32_t>(EHCIRequestState::Cancelled), __ATOMIC_RELEASE);
    return true;
}

bool ehci_request_poll(uint32_t request_id, EHCIRequestSnapshot* out) {
    if (!out || !request_id) return false;
    for (uint32_t attempt = 0; attempt < 4u; ++attempt) {
        EHCIRequestSlot* slot = request_find(request_id);
        if (!slot) return false;
        const uint32_t state_before = request_state_load(slot);
        if (state_before == static_cast<uint32_t>(EHCIRequestState::Reserving) ||
            state_before == static_cast<uint32_t>(EHCIRequestState::Completing) ||
            state_before == static_cast<uint32_t>(EHCIRequestState::Free)) continue;
        const uint32_t id_before = __atomic_load_n(&slot->request_id, __ATOMIC_ACQUIRE);
        const uint32_t result = __atomic_load_n(&slot->result, __ATOMIC_RELAXED);
        const uint32_t transferred = __atomic_load_n(&slot->transferred, __ATOMIC_RELAXED);
        const uint32_t state_after = request_state_load(slot);
        const uint32_t id_after = __atomic_load_n(&slot->request_id, __ATOMIC_ACQUIRE);
        if (state_before != state_after || id_before != id_after || id_after != request_id) continue;
        out->request_id = request_id;
        out->state = static_cast<EHCIRequestState>(state_after);
        out->result = static_cast<EHCIRequestResult>(result);
        out->transferred = static_cast<uint16_t>(transferred <= 0xFFFFu ? transferred : 0u);
        return true;
    }
    return false;
}

bool ehci_request_wait(uint32_t request_id, uint32_t timeout_ms,
                       EHCIRequestSnapshot* out) {
    if (!out || request_id == 0u) return false;
    out->request_id = request_id;
    out->state = EHCIRequestState::Free;
    out->result = EHCIRequestResult::Pending;
    out->transferred = 0u;
    if (!ehci_request_poll(request_id, out)) return false;
    if (starlix::drivers::ehci::async_request_policy::request_state_terminal(
            static_cast<uint32_t>(out->state))) return true;
    if (timeout_ms == 0u ||
        !starlix::drivers::ehci::async_request_policy::wait_can_block(
            starlix::cpu::timer_is_ready(), starlix::cpu::interrupts_enabled()))
        return false;

    const uint64_t deadline = starlix::drivers::ehci::async_request_policy::deadline_after(
        starlix::cpu::timer_now_ms(), timeout_ms);
    for (;;) {
        (void)ehci_service_deferred_work();
        if (!ehci_request_poll(request_id, out)) return false;
        if (starlix::drivers::ehci::async_request_policy::request_state_terminal(
                static_cast<uint32_t>(out->state))) return true;
        if (starlix::drivers::ehci::async_request_policy::deadline_reached(
                starlix::cpu::timer_now_ms(), deadline))
            return false; /* Request stays owned; caller must keep the buffer alive. */
        asm volatile("sti; hlt" ::: "memory");
    }
}

bool ehci_request_release(uint32_t request_id) {
    EHCIRequestSlot* slot = request_find(request_id);
    if (!slot) return false;
    uint32_t state = request_state_load(slot);
    if (!starlix::drivers::ehci::request_policy::terminal_state(state)) return false;
    if (!__atomic_compare_exchange_n(&slot->state, &state,
            static_cast<uint32_t>(EHCIRequestState::Reserving), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&slot->request_id, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->result, static_cast<uint32_t>(EHCIRequestResult::Pending), __ATOMIC_RELAXED);
    __atomic_store_n(&slot->transferred, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->cancel_requested, 0u, __ATOMIC_RELAXED);
    mem_zero(&slot->transfer, sizeof(slot->transfer));
    __atomic_store_n(&slot->state, static_cast<uint32_t>(EHCIRequestState::Free), __ATOMIC_RELEASE);
    return true;
}

static bool ehci_request_lifecycle_self_test() {
    if (!ehci_request_queue_init()) return false;
    EHCITransferRequest request{};
    request.type = EHCIRequestType::Bulk;
    request.device_address = 1u;
    request.endpoint_address = 0x81u;
    request.max_packet_size = 512u;

    /* Software-only admission cases run even when no USB device is attached. */
    EHCITransferRequest invalid = request;
    invalid.data_length = static_cast<uint16_t>(EHCI_MAX_TRANSFER_BYTES + 1u);
    invalid.data_buffer = ehci_request_smoke_data;
    if (request_validate(&invalid)) return false;
    invalid = request;
    invalid.data_length = 1u;
    invalid.data_buffer = nullptr;
    if (request_validate(&invalid)) return false;
    invalid = request;
    invalid.endpoint_address = 0x91u; /* reserved endpoint-address bits */
    if (request_validate(&invalid)) return false;
    invalid = request;
    invalid.max_packet_size = 1024u; /* EHCI high-speed bulk maximum is 512 */
    if (request_validate(&invalid)) return false;
    invalid = request;
    invalid.type = static_cast<EHCIRequestType>(0u);
    if (request_validate(&invalid)) return false;

    uint32_t ids[EHCI_REQUEST_CAPACITY]{};
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        if (!ehci_request_submit(&request, &ids[i])) return false;
    }
    uint32_t overflow_id = 0;
    if (ehci_request_submit(&request, &overflow_id)) return false;

    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        EHCIQueuedRequest queued{};
        if (!ehci_request_dequeue(&queued) || queued.state != EHCIRequestState::Active) return false;
        if (!ehci_request_complete(queued.request_id, EHCIRequestResult::Success, 0u)) return false;
    }
    for (uint32_t i = 0; i < EHCI_REQUEST_CAPACITY; ++i) {
        EHCIRequestSnapshot status{};
        if (!ehci_request_poll(ids[i], &status) ||
            status.state != EHCIRequestState::Completed ||
            status.result != EHCIRequestResult::Success ||
            !ehci_request_release(ids[i])) return false;
    }
    uint32_t cancel_id = 0;
    EHCIRequestSnapshot cancelled{};
    if (!ehci_request_submit(&request, &cancel_id) || !ehci_request_cancel(cancel_id) ||
        !ehci_request_poll(cancel_id, &cancelled) ||
        cancelled.state != EHCIRequestState::Cancelled ||
        cancelled.result != EHCIRequestResult::Cancelled ||
        !ehci_request_release(cancel_id)) return false;

    /* Phase 6D-4: a request's borrowed data_buffer pointer and slot metadata
       must not be discarded while a worker can still consume them. Queued
       requests cannot be released; active requests cannot be cancelled or
       released; terminal completion is required before release invalidates ID. */
    uint32_t ownership_id = 0u;
    if (!ehci_request_submit(&request, &ownership_id)) return false;
    if (ehci_request_release(ownership_id)) return false; /* still Queued */

    EHCIQueuedRequest active_request{};
    if (!ehci_request_dequeue(&active_request) ||
        active_request.request_id != ownership_id ||
        active_request.state != EHCIRequestState::Active) return false;
    if (ehci_request_cancel(ownership_id)) return false; /* Manually Active, not owned by hardware engine */
    if (ehci_request_release(ownership_id)) return false; /* worker still owns slot */

    if (!ehci_request_complete(ownership_id, EHCIRequestResult::TransferError, 0u)) return false;
    if (ehci_request_complete(ownership_id, EHCIRequestResult::Success, 0u)) return false;
    if (ehci_request_cancel(ownership_id)) return false; /* terminal state is immutable */
    EHCIRequestSnapshot ownership_status{};
    if (!ehci_request_poll(ownership_id, &ownership_status) ||
        ownership_status.state != EHCIRequestState::Failed ||
        ownership_status.result != EHCIRequestResult::TransferError) return false;
    if (!ehci_request_release(ownership_id)) return false;
    if (ehci_request_poll(ownership_id, &ownership_status)) return false; /* ID invalidated */
    return true;
}

static bool ehci_request_transfer_smoke_test() {
    if (!controller_active) return false;
    if (!starlix::cpu::timer_is_ready()) {
        starlix::drivers::serial::print(
            "[EHCI][REQ] queued transfer self-test skipped (PIT timer unavailable).\n");
        return true;
    }

    uint8_t device_address = 0u;
    for (uint16_t address = 1u; address < 128u; ++address) {
        if (device_ep0_max_packet[address] != 0u) {
            device_address = static_cast<uint8_t>(address);
            break;
        }
    }
    if (!device_address) {
        starlix::drivers::serial::print("[EHCI][REQ] queued transfer self-test skipped (no enumerated USB device).\n");
        return true;
    }

    /* Standard device GET_CONFIGURATION request verifies that a queued request
       reaches the real EHCI control-transfer path and publishes completion. */
    uint8_t configuration_value = 0u;
    EHCITransferRequest request{};
    request.type = EHCIRequestType::Control;
    request.device_address = device_address;
    request.endpoint_address = 0u;
    request.max_packet_size = device_ep0_max_packet[device_address];
    request.setup.bmRequestType = 0x80u; /* Device-to-host, standard, device. */
    request.setup.bRequest = 8u;         /* GET_CONFIGURATION */
    request.setup.wValue = 0u;
    request.setup.wIndex = 0u;
    request.setup.wLength = 1u;
    request.data_buffer = &configuration_value;
    request.data_length = 1u;

    uint32_t request_id = 0u;
    if (!ehci_request_submit(&request, &request_id)) return false;
    EHCIRequestSnapshot snapshot{};
    /* Use the timer-backed wait API rather than a service-poll-count loop. This
       lets IRQ0 wake the CPU while deferred EHCI completion and IAA reclamation
       proceed in non-IRQ context. */
    const bool polled = ehci_request_wait(request_id,
        EHCI_ASYNC_REQUEST_TIMEOUT_MS + EHCI_ASYNC_IAA_TIMEOUT_MS + 100u, &snapshot);
    const bool passed = polled &&
        snapshot.state == EHCIRequestState::Completed &&
        snapshot.result == EHCIRequestResult::Success &&
        snapshot.transferred == 1u && configuration_value != 0u;
    if (polled && (snapshot.state == EHCIRequestState::Completed ||
                   snapshot.state == EHCIRequestState::Failed ||
                   snapshot.state == EHCIRequestState::Cancelled)) {
        (void)ehci_request_release(request_id);
    }
    if (!passed) {
        starlix::drivers::serial::print("[EHCI][REQ] queued control-transfer execution self-test failed.\n");
        return false;
    }
    starlix::drivers::serial::print("[EHCI][REQ] queued control-transfer execution self-test passed.\n");

    /* QEMU's built-in USB device models cap a single control request buffer at
       4096 bytes. Asking for 8192 bytes causes a device-model error and a host
       timeout, which can stop the controller and break later storage tests.
       Exercise the multi-qTD path with a valid 16 KiB MSC READ(10) bulk-IN
       transaction instead. The public block API remains capped at 4 KiB. */
    USB_MSC_DEVICE* test_msc = nullptr;
    for (uint16_t address = 1u; address < 128u; ++address) {
        if (usb_msc_devices[address].present && usb_msc_devices[address].block_size != 0u) {
            test_msc = &usb_msc_devices[address];
            break;
        }
    }
    if (!test_msc) {
        starlix::drivers::serial::print(
            "[EHCI][REQ] multi-qTD bulk READ(10) test skipped (no MSC device).\n");
        return true;
    }

    const uint32_t block_size = test_msc->block_size;
    if (block_size == 0u || block_size > EHCI_MAX_TRANSFER_BYTES ||
        (EHCI_MAX_TRANSFER_BYTES % block_size) != 0u) {
        starlix::drivers::serial::print(
            "[EHCI][REQ] multi-qTD bulk READ(10) test skipped (unsupported block size).\n");
        return true;
    }
    const uint16_t block_count = static_cast<uint16_t>(EHCI_MAX_TRANSFER_BYTES / block_size);
    if (block_count == 0u || test_msc->last_lba < static_cast<uint32_t>(block_count - 1u)) {
        starlix::drivers::serial::print(
            "[EHCI][REQ] multi-qTD bulk READ(10) test skipped (device too small).\n");
        return true;
    }

    mem_zero(ehci_request_smoke_data, sizeof(ehci_request_smoke_data));
    uint8_t read_cdb[10]{};
    read_cdb[0] = 0x28u; /* READ(10), starting at LBA zero; read-only test. */
    write_be32(read_cdb + 2u, 0u);
    write_be16(read_cdb + 7u, block_count);
    const bool multi_passed = msc_bot_command(*test_msc, read_cdb, sizeof(read_cdb), true,
                                               ehci_request_smoke_data,
                                               EHCI_MAX_TRANSFER_BYTES);
    starlix::drivers::serial::print(multi_passed
        ? "[EHCI][REQ] multi-qTD bulk READ(10) test passed (16384 bytes).\n"
        : "[EHCI][REQ] multi-qTD bulk READ(10) test FAILED.\n");
    return multi_passed;
}

bool ehci_irq_take_events(EHCIIRQEvents* out) {
    if (!out) return false;
    /* The latch exchange is the handoff point: causes racing with this drain
       remain latched for the next worker pass. Counters are cumulative snapshots. */
    out->pending_status = ehci_irq_event_latch.take_pending();
    out->interrupt_count = ehci_irq_event_latch.interrupt_count();
    out->async_advance_count = ehci_irq_event_latch.auxiliary_count();
    return true;
}

bool ehci_usb_get_device_info(uint8_t dev_addr, EHCIUSBDeviceInfo* out) {
    if (!out || dev_addr == 0u || dev_addr >= 128u) return false;
    const USBDeviceModelRecord& record = usb_device_model[dev_addr];
    if (!starlix::drivers::ehci::device_model_policy::query_allowed(
            record.present,
            static_cast<starlix::drivers::ehci::device_model_policy::State>(
                usb_device_lifecycle_state[dev_addr]),
            dev_addr, record.info.generation, usb_device_generation[dev_addr])) return false;
    *out = record.info;
    return true;
}

bool ehci_msc_get_device_info(uint8_t dev_addr, MSCDeviceInfo* out) {
    if (!out || dev_addr == 0u || dev_addr >= 128u) return false;
    const USB_MSC_DEVICE& device = usb_msc_devices[dev_addr];
    if (!starlix::drivers::ehci::recovery_policy::allow_block_io(
            device.present, static_cast<starlix::drivers::ehci::recovery_policy::DeviceState>(
                __atomic_load_n(&device.recovery_state, __ATOMIC_ACQUIRE)), controller_active) ||
        device.block_size == 0u) return false;
    out->last_lba = device.last_lba;
    out->block_size = device.block_size;
    return true;
}

} // namespace starlix::drivers::ehci
