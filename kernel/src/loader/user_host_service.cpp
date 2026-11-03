// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "user_host_service.hpp"
#include "user_host_service_policy.hpp"
#include "../../../user_components/stlcs/stlcs_abi.hpp"
#include "../cpu/idt.hpp"
#include "../cpu/gdt.hpp"
#include "../cpu/interrupt_frame.hpp"
#include "../process/process.hpp"
#include "../process/context_switch.hpp"
#include "../mem/address_space.hpp"
#include "../mem/kernel_stack.hpp"
#include "../mem/user_address.hpp"
#include "../drivers/ehci.hpp"
#include "../gui/keyboard_text.hpp"
#include "../drivers/framebuffer.hpp"
#include "../drivers/serial.hpp"

namespace starlix::loader::host_service {
namespace {

using starlix::stlcs::AuthResult;
using starlix::stlcs::InputEvent;
using starlix::stlcs::InputKind;
using starlix::stlcs::KeyCode;
using starlix::process::StProcess;
using starlix::process::StThread;
using starlix::cpu::InterruptReturnFrame;
using starlix::mem::AddressSpace;
using namespace starlix::mem::user_address;

constexpr uint64_t USER_COPY_LIMIT = 16384ULL;
constexpr uint32_t DISPLAY_TILE_MAX =
    starlix::stlcs::DISPLAY_TILE_MAX_PIXELS;
static_assert(sizeof(uint32_t) * DISPLAY_TILE_MAX <= 4096u,
              "DisplayBlit scratch must not consume the entire 16 KiB kernel stack");
constexpr uint64_t PHYSICAL_IDENTITY_LIMIT = 0x100000000ULL;

StThread* g_lock_screen_thread = nullptr;
uint32_t g_caps_lock = 0u;
uint32_t g_active_logged = 0u;
volatile uint32_t g_service_trace_calls = 0u;
uint32_t g_first_blit_trace = 0u;

void serial_line(const char* text) {
    starlix::drivers::serial::print(text);
    starlix::drivers::serial::print("\r\n");
}

void secure_zero(char* bytes, uint32_t length) {
    volatile char* output = bytes;
    for (uint32_t i = 0u; i < length; ++i) output[i] = 0;
}

bool valid_ascii_string(const char* value, uint32_t capacity,
                        bool allow_empty = true) {
    if (!value || capacity == 0u) return false;
    uint32_t length = 0u;
    while (length < capacity && value[length] != '\0') {
        const uint8_t ch = static_cast<uint8_t>(value[length]);
        if (ch < 0x20u || ch > 0x7Eu) return false;
        ++length;
    }
    return length < capacity && (allow_empty || length != 0u);
}

bool copy_user_bytes(uint64_t root, uint64_t user_address,
                     void* kernel_buffer, uint64_t length,
                     bool write_to_user) {
    if (!kernel_buffer || length == 0u || length > USER_COPY_LIMIT ||
        !is_user_range(user_address, length)) return false;

    AddressSpace space{root, 0u, starlix::mem::ADDRESS_SPACE_MAGIC};
    auto* local = static_cast<uint8_t*>(kernel_buffer);
    uint64_t current = user_address;
    uint64_t completed = 0u;
    while (completed < length) {
        uint64_t physical = 0u;
        uint32_t permissions = 0u;
        if (!starlix::mem::address_space_translate_user(
                space, current, physical, permissions)) return false;
        if ((permissions & USER_PAGE_READ) == 0u) return false;
        if (write_to_user && (permissions & USER_PAGE_WRITE) == 0u) return false;
        if (physical >= PHYSICAL_IDENTITY_LIMIT) return false;

        const uint64_t page_remaining = PAGE_SIZE - (current & (PAGE_SIZE - 1u));
        uint64_t chunk = length - completed;
        if (chunk > page_remaining) chunk = page_remaining;
        if (chunk > PHYSICAL_IDENTITY_LIMIT - physical) return false;
        auto* mapped = reinterpret_cast<volatile uint8_t*>(
            static_cast<uintptr_t>(physical));
        for (uint64_t i = 0u; i < chunk; ++i) {
            if (write_to_user) mapped[i] = local[completed + i];
            else local[completed + i] = mapped[i];
        }
        current += chunk;
        completed += chunk;
    }
    return true;
}

bool current_lock_screen_owner(InterruptReturnFrame* frame,
                               StThread*& out_thread,
                               StProcess*& out_process) {
    out_thread = nullptr;
    out_process = nullptr;
    if (!frame || frame->vector != 0x81ULL ||
        (frame->cs & 3ULL) != 3ULL ||
        !starlix::cpu::valid_user_interrupt_return_frame(*frame) ||
        !starlix::process::context_switch::context_switch_armed()) return false;

    StThread* thread = starlix::process::context_switch::context_switch_current();
    if (!thread || thread != __atomic_load_n(&g_lock_screen_thread, __ATOMIC_ACQUIRE) ||
        !starlix::process::thread_is_registered(thread) ||
        thread->state != starlix::process::THREAD_STATE_RUNNING ||
        !thread->parent_process) return false;

    StProcess* process = thread->parent_process;
    if (!starlix::process::process_is_registered(process) ||
        process->state != starlix::process::PROCESS_STATE_RUNNING ||
        process->main_thread != thread ||
        process->page_directory_cr3 == 0ULL ||
        starlix::mem::address_space_active_root() != process->page_directory_cr3 ||
        starlix::cpu::tss_get_kernel_stack() != thread->kernel_stack_top ||
        !starlix::mem::kernel_stack_thread_valid(
            thread, process->page_directory_cr3)) return false;

    static constexpr char expected_name[] = "stlcs";
    for (uint32_t i = 0u; i < sizeof(expected_name); ++i)
        if (process->image_name[i] != expected_name[i]) return false;

    uint64_t code_physical = 0u;
    uint32_t code_permissions = 0u;
    if (!starlix::mem::address_space_translate_user(
            AddressSpace{process->page_directory_cr3, 0u,
                         starlix::mem::ADDRESS_SPACE_MAGIC},
            frame->rip, code_physical, code_permissions) ||
        (code_permissions & USER_PAGE_EXEC) == 0u) return false;

    uint64_t stack_physical = 0u;
    uint32_t stack_permissions = 0u;
    if (frame->rsp == 0u ||
        !starlix::mem::address_space_translate_user(
            AddressSpace{process->page_directory_cr3, 0u,
                         starlix::mem::ADDRESS_SPACE_MAGIC},
            frame->rsp - 1u, stack_physical, stack_permissions) ||
        (stack_permissions & USER_PAGE_WRITE) == 0u ||
        (stack_permissions & USER_PAGE_EXEC) != 0u) return false;

    out_thread = thread;
    out_process = process;
    return true;
}

bool map_hid_key(const starlix::drivers::ehci::EHCIInputEvent& source,
                 InputEvent& output) {
    using starlix::drivers::ehci::EHCIInputEventType;
    if (source.type != EHCIInputEventType::KeyDown) return false;
    output = InputEvent{};
    output.kind = InputKind::KeyDown;

    const uint8_t modifiers = source.modifiers;
    if ((modifiers & 0x11u) != 0u)
        output.modifiers |= starlix::stlcs::ModifierCtrl;
    if ((modifiers & 0x44u) != 0u)
        output.modifiers |= starlix::stlcs::ModifierAlt;
    if ((modifiers & 0x88u) != 0u)
        output.modifiers |= starlix::stlcs::ModifierMeta;

    if (source.code >= 0xE0u && source.code <= 0xE7u) return false;
    if (source.code == 0x39u) {
        g_caps_lock ^= 1u;
        return false;
    }
    if (source.code == 0x28u) {
        output.key = KeyCode::Enter;
        return true;
    }
    if (source.code == 0x2Au) {
        output.key = KeyCode::Backspace;
        return true;
    }
    if (source.code == 0x29u) {
        output.key = KeyCode::Escape;
        return true;
    }

    const bool shifted = (modifiers & 0x22u) != 0u;
    const char ch = starlix::gui::keyboard_text::usage_to_ascii(
        source.code, shifted, g_caps_lock != 0u);
    if (ch == '\0') return false;
    output.key = KeyCode::Character;
    output.codepoint = static_cast<uint32_t>(static_cast<uint8_t>(ch));
    return true;
}

uint64_t service_poll_input(InterruptReturnFrame& frame,
                            const StProcess& process) {
    starlix::drivers::ehci::EHCIInputEvent raw{};
    InputEvent mapped{};
    for (uint32_t attempt = 0u; attempt < 32u; ++attempt) {
        (void)starlix::drivers::ehci::ehci_hid_service();
        if (!starlix::drivers::ehci::ehci_hid_event_poll(&raw)) return 0u;
        if (!map_hid_key(raw, mapped)) continue;
        if (!copy_user_bytes(process.page_directory_cr3, frame.rsi,
                             &mapped, sizeof(mapped), true)) return ~0ULL;
        return 1u;
    }
    return 0u;
}

uint64_t service_display_info(InterruptReturnFrame& frame,
                              const StProcess& process) {
    starlix::drivers::framebuffer::FramebufferInfo kernel_info{};
    if (!starlix::drivers::framebuffer::query(kernel_info)) return 0u;
    starlix::stlcs::DisplayInfo user_info{};
    user_info.struct_size = sizeof(user_info);
    user_info.width = kernel_info.width;
    user_info.height = kernel_info.height;
    user_info.pixels_per_scanline = kernel_info.pixels_per_scanline;
    user_info.pixel_format = kernel_info.pixel_format;
    user_info.reserved = 0u;
    return copy_user_bytes(process.page_directory_cr3, frame.rsi,
        &user_info, sizeof(user_info), true) ? 1u : ~0ULL;
}

uint64_t service_display_blit(InterruptReturnFrame& frame,
                              const StProcess& process) {
    starlix::stlcs::DisplayBlit request{};
    if (!copy_user_bytes(process.page_directory_cr3, frame.rsi,
            &request, sizeof(request), false) ||
        request.struct_size != sizeof(request) || request.reserved != 0u ||
        request.width == 0u || request.height == 0u ||
        request.width > starlix::stlcs::DISPLAY_TILE_EDGE ||
        request.height > starlix::stlcs::DISPLAY_TILE_EDGE ||
        request.source_stride < request.width ||
        request.source_stride > starlix::stlcs::DISPLAY_TILE_EDGE ||
        request.pixels == 0ULL) return ~0ULL;

    const uint64_t source_pixels =
        static_cast<uint64_t>(request.height) * request.source_stride;
    if (source_pixels == 0ULL || source_pixels > DISPLAY_TILE_MAX ||
        source_pixels > USER_COPY_LIMIT / sizeof(uint32_t)) return ~0ULL;

    uint32_t pixels[DISPLAY_TILE_MAX]{}; // 4 KiB maximum, not a full 64x64 tile.
    if (!copy_user_bytes(process.page_directory_cr3, request.pixels,
            pixels, source_pixels * sizeof(uint32_t), false)) return ~0ULL;

    return starlix::drivers::framebuffer::blit(
        request.x, request.y, request.width, request.height,
        request.source_stride, pixels) ? 1u : 0u;
}

uint64_t service_frame_complete() {
    if (!starlix::drivers::framebuffer::ready()) return 0u;
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&g_active_logged, &expected, 1u, false,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        serial_line("[RING3] STLCS_ENTRY=EXECUTING");
        serial_line("[LOCKSCREEN] COMPONENT_ACTIVE=PASS");
    }
    return 1u;
}

uint64_t service_monotonic_milliseconds() {
    if (!starlix::cpu::timer_is_ready()) return ~0ULL;
    return starlix::cpu::timer_now_ms();
}

uint64_t service_authenticate(InterruptReturnFrame& frame,
                              const StProcess& process) {
    char user_name[starlix::stlcs::USER_NAME_CAPACITY]{};
    char password[starlix::stlcs::PASSWORD_CAPACITY]{};
    const uint64_t length = frame.rcx;
    bool valid = length < starlix::stlcs::PASSWORD_CAPACITY &&
        copy_user_bytes(process.page_directory_cr3, frame.rsi,
                        user_name, sizeof(user_name), false) &&
        valid_ascii_string(user_name, sizeof(user_name), false) &&
        copy_user_bytes(process.page_directory_cr3, frame.rdx,
                        password, length + 1u, false) &&
        password[length] == '\0';
    if (valid) {
        for (uint64_t i = 0u; i < length; ++i) {
            const uint8_t ch = static_cast<uint8_t>(password[i]);
            if (ch < 0x20u || ch > 0x7Eu) valid = false;
        }
    }
    /* No lsass.ect/trusted authentication endpoint is installed yet. Credentials
       are validated transiently, never logged, then wiped before returning. */
    (void)valid;
    secure_zero(password, sizeof(password));
    secure_zero(user_name, sizeof(user_name));
    return policy::auth_service_unavailable_result();
}

uint64_t service_request_desktop(InterruptReturnFrame& frame,
                                 const StProcess& process) {
    char user_name[starlix::stlcs::USER_NAME_CAPACITY]{};
    const bool valid = copy_user_bytes(process.page_directory_cr3, frame.rsi,
        user_name, sizeof(user_name), false) &&
        valid_ascii_string(user_name, sizeof(user_name), false);
    secure_zero(user_name, sizeof(user_name));
    /* Desktop handoff remains denied until a trusted auth token and session
       manager exist. A component cannot unlock itself by calling this gate. */
    (void)valid;
    return policy::desktop_handoff_denied_result();
}

} // namespace

bool bind_lock_screen_thread(StThread* thread) {
    if (!thread || !starlix::process::thread_is_registered(thread) ||
        !thread->parent_process ||
        thread->parent_process->state != starlix::process::PROCESS_STATE_READY ||
        thread->state != starlix::process::THREAD_STATE_READY ||
        !service_gate_ready()) return false;
    StThread* expected = nullptr;
    const bool bound = __atomic_compare_exchange_n(
        &g_lock_screen_thread, &expected, thread, false,
        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    return bound;
}

void clear_lock_screen_thread(StThread* thread) {
    if (!thread) return;
    StThread* expected = thread;
    (void)__atomic_compare_exchange_n(&g_lock_screen_thread, &expected,
        static_cast<StThread*>(nullptr), false,
        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

bool service_gate_ready() {
    return starlix::cpu::user_host_service_gate_ready() &&
           starlix::drivers::framebuffer::ready();
}

} // namespace starlix::loader::host_service

extern "C" void starlix_user_service_dispatch(void* opaque_frame) {
    using namespace starlix::loader::host_service;
    auto* frame = static_cast<starlix::cpu::InterruptReturnFrame*>(opaque_frame);
    if (!frame) {
        serial_line("[RING3] HOSTAPI_FRAME=NULL");
        return;
    }
    const uint32_t trace_call = __atomic_fetch_add(
        &g_service_trace_calls, 1u, __ATOMIC_ACQ_REL);
    const uint64_t service_id = frame->rax;
    frame->rax = ~0ULL;
    if (trace_call == 0u)
        serial_line("[RING3] HOSTAPI_GATE=ENTERED");

    starlix::process::StThread* thread = nullptr;
    starlix::process::StProcess* process = nullptr;
    if (!current_lock_screen_owner(frame, thread, process)) {
        if (trace_call < 4u)
            serial_line("[RING3] HOSTAPI_OWNER=REJECTED");
        return;
    }
    if (trace_call == 0u)
        serial_line("[RING3] HOSTAPI_OWNER=PASS");
    if (!starlix::loader::host_service::policy::service_abi_version_valid(
            frame->rdi)) {
        if (trace_call < 4u)
            serial_line("[RING3] HOSTAPI_ABI=REJECTED");
        return;
    }

    uint64_t result = ~0ULL;
    if (!starlix::loader::host_service::policy::service_allowed(service_id)) {
        if (trace_call < 4u)
            serial_line("[RING3] HOSTAPI_POLICY=REJECTED");
        return;
    }
    switch (static_cast<starlix::loader::host_service::policy::Service>(service_id)) {
        case starlix::loader::host_service::policy::Service::PollInput:
            result = service_poll_input(*frame, *process); break;
        case starlix::loader::host_service::policy::Service::DisplayInfo:
            result = service_display_info(*frame, *process);
            if (trace_call == 0u)
                serial_line(result == 1u
                    ? "[RING3] DISPLAY_INFO=PASS"
                    : "[RING3] DISPLAY_INFO=FAIL");
            break;
        case starlix::loader::host_service::policy::Service::DisplayBlit:
            result = service_display_blit(*frame, *process);
            if (result == 1u) {
                uint32_t expected = 0u;
                if (__atomic_compare_exchange_n(&g_first_blit_trace, &expected,
                        1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                    serial_line("[RING3] FIRST_DISPLAY_BLIT=PASS");
            } else {
                uint32_t expected = 0u;
                if (__atomic_compare_exchange_n(&g_first_blit_trace, &expected,
                        2u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                    serial_line("[RING3] FIRST_DISPLAY_BLIT=FAIL");
            }
            break;
        case starlix::loader::host_service::policy::Service::FrameComplete:
            result = service_frame_complete(); break;
        case starlix::loader::host_service::policy::Service::MonotonicMilliseconds:
            result = service_monotonic_milliseconds(); break;
        case starlix::loader::host_service::policy::Service::Authenticate:
            result = service_authenticate(*frame, *process); break;
        case starlix::loader::host_service::policy::Service::RequestDesktop:
            result = service_request_desktop(*frame, *process); break;
        case starlix::loader::host_service::policy::Service::Yield:
            /* Yield to the PIT without spinning. The interrupt gate entered
               with IF clear; STI;HLT is the bounded idle primitive here. */
            asm volatile("sti; hlt; cli" ::: "memory");
            result = 0u;
            break;
        default:
            if (trace_call < 4u)
                serial_line("[RING3] HOSTAPI_SERVICE=UNKNOWN");
            return;
    }
    frame->rax = result;
}
