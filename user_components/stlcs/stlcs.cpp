// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "stlcs_abi.hpp"
#include "font8x8.hpp"

// Minimal freestanding C-runtime primitive emitted for aggregate zero-init.
// Volatile stores prevent the compiler from folding this loop back into memset.
extern "C" void* memset(void* destination, int value, __SIZE_TYPE__ count) {
    auto* bytes = static_cast<volatile unsigned char*>(destination);
    for (__SIZE_TYPE__ i = 0; i < count; ++i)
        bytes[i] = static_cast<unsigned char>(value);
    return destination;
}

namespace starlix::stlcs {
namespace {

constexpr stlcs_u32 MAX_INPUT_EVENTS_PER_STEP = 32u;

stlcs_u32 text_length(const char* text, stlcs_u32 limit) {
    if (!text) return 0u;
    stlcs_u32 length = 0u;
    while (length < limit && text[length] != '\0') ++length;
    return length;
}

void clear_bytes(char* destination, stlcs_u32 capacity) {
    if (!destination) return;
    volatile char* output = destination;
    for (stlcs_u32 i = 0u; i < capacity; ++i) output[i] = 0;
}

void copy_text(char* destination, stlcs_u32 capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    clear_bytes(destination, capacity);
    if (!source) return;
    stlcs_u32 i = 0u;
    while (i + 1u < capacity && source[i] != '\0') {
        destination[i] = source[i];
        ++i;
    }
    destination[i] = '\0';
}

bool bytes_equal(const void* left, const void* right, stlcs_u32 length) {
    const auto* a = static_cast<const unsigned char*>(left);
    const auto* b = static_cast<const unsigned char*>(right);
    for (stlcs_u32 i = 0u; i < length; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

void copy_bytes(void* destination, const void* source, stlcs_u32 length) {
    auto* out = static_cast<volatile unsigned char*>(destination);
    const auto* in = static_cast<const unsigned char*>(source);
    for (stlcs_u32 i = 0u; i < length; ++i) out[i] = in[i];
}

bool bounded_name_valid(const char* name) {
    if (!name) return false;
    const stlcs_u32 length = text_length(name, USER_NAME_CAPACITY);
    if (length == 0u || length >= USER_NAME_CAPACITY) return false;
    for (stlcs_u32 i = 0u; i < length; ++i) {
        const stlcs_u8 value = static_cast<stlcs_u8>(name[i]);
        if (value < 0x20u || value > 0x7Eu) return false;
    }
    return true;
}

bool host_api_valid(const HostApi* host) {
    return host && host->abi_version == ABI_VERSION &&
        host->struct_size >= sizeof(HostApi) && host->poll_input &&
        host->get_clock && host->present && host->authenticate &&
        host->request_desktop && host->yield && host->get_monotonic_ms;
}

void set_status(Session* session, const char* status) {
    copy_text(session->status_text, STATUS_CAPACITY, status);
}

void clear_password(Session* session) {
    clear_bytes(session->password, PASSWORD_CAPACITY);
    session->password_length = 0u;
}

void refresh_clock(Session* session) {
    if (!session->host->get_clock(session->host->context,
                                  session->clock_text, CLOCK_CAPACITY,
                                  session->date_text, DATE_CAPACITY)) {
        copy_text(session->clock_text, CLOCK_CAPACITY, "--:--:--");
        copy_text(session->date_text, DATE_CAPACITY, "Date unavailable");
    }
    session->clock_text[CLOCK_CAPACITY - 1u] = '\0';
    session->date_text[DATE_CAPACITY - 1u] = '\0';
}

void update_transition(Session* session) {
    if (!session || session->transition_complete != 0u) return;
    const stlcs_u64 now = session->host->get_monotonic_ms(session->host->context);
    if (now == MONOTONIC_UNAVAILABLE || now < session->transition_start_ms) {
        /* A missing or regressing clock skips the visual effect rather than
           inventing elapsed time or leaving the Lock Screen permanently black. */
        session->transition_alpha = 255u;
        session->transition_complete = 1u;
        return;
    }
    session->transition_alpha = lock_screen_fade_alpha(
        now - session->transition_start_ms);
    if (session->transition_alpha >= 255u) {
        session->transition_alpha = 255u;
        session->transition_complete = 1u;
    }
}

bool present(Session* session) {
    update_transition(session);
    Presentation frame{};
    frame.struct_size = sizeof(Presentation);
    frame.state = static_cast<stlcs_u32>(session->state);
    copy_text(frame.user_name, USER_NAME_CAPACITY, session->user_name);
    copy_text(frame.clock_text, CLOCK_CAPACITY, session->clock_text);
    copy_text(frame.date_text, DATE_CAPACITY, session->date_text);
    copy_text(frame.status_text, STATUS_CAPACITY, session->status_text);
    frame.transition_alpha = session->transition_alpha;
    const stlcs_u32 visible = session->password_length < PASSWORD_CAPACITY - 1u
        ? session->password_length : PASSWORD_CAPACITY - 1u;
    for (stlcs_u32 i = 0u; i < visible; ++i) frame.password_mask[i] = '*';
    frame.password_mask[visible] = '\0';
    if (session->has_presentation != 0u &&
        bytes_equal(&frame, &session->last_presentation, sizeof(frame)))
        return true;
    if (!session->host->present(session->host->context, &frame)) return false;
    copy_bytes(&session->last_presentation, &frame, sizeof(frame));
    session->has_presentation = 1u;
    return true;
}

void authenticate(Session* session) {
    if (session->password_length == 0u) {
        set_status(session, "Enter your password");
        return;
    }

    // Copy to an ephemeral stack buffer, clear persistent session storage
    // before crossing the authentication boundary, then wipe the copy.
    char ephemeral[PASSWORD_CAPACITY]{};
    const stlcs_u32 length = session->password_length;
    for (stlcs_u32 i = 0u; i < length; ++i)
        ephemeral[i] = session->password[i];
    ephemeral[length] = '\0';
    clear_password(session);
    set_status(session, "Checking password...");

    const AuthResult result = session->host->authenticate(
        session->host->context, session->user_name, ephemeral, length);
    clear_bytes(ephemeral, PASSWORD_CAPACITY);

    if (result == AuthResult::Rejected) {
        set_status(session, "Incorrect password. Try again.");
        return;
    }
    if (result != AuthResult::Accepted) {
        set_status(session, "Authentication service unavailable");
        return;
    }

    set_status(session, "Starting your desktop...");
    if (session->host->request_desktop(session->host->context,
                                       session->user_name)) {
        session->state = SessionState::DesktopRequested;
    } else {
        set_status(session, "Desktop session could not start");
    }
}

void handle_input(Session* session, const InputEvent& event) {
    if (event.kind != InputKind::KeyDown ||
        (event.modifiers & (ModifierCtrl | ModifierAlt | ModifierMeta)) != 0u)
        return;

    if (event.key == KeyCode::Enter ||
        (event.key == KeyCode::Character && event.codepoint == '\r')) {
        authenticate(session);
        return;
    }

    if (event.key == KeyCode::Backspace ||
        (event.key == KeyCode::Character && event.codepoint == '\b')) {
        if (session->password_length != 0u) {
            --session->password_length;
            session->password[session->password_length] = '\0';
        }
        set_status(session, "Press ENTER to unlock");
        return;
    }

    if (event.key == KeyCode::Escape ||
        (event.key == KeyCode::Character && event.codepoint == 0x1Bu)) {
        clear_password(session);
        set_status(session, "Password cleared");
        return;
    }

    if (event.key != KeyCode::Character ||
        event.codepoint < 0x20u || event.codepoint > 0x7Eu ||
        session->password_length >= PASSWORD_CAPACITY - 1u)
        return;

    session->password[session->password_length++] =
        static_cast<char>(event.codepoint);
    session->password[session->password_length] = '\0';
    set_status(session, "Press ENTER to unlock");
}

} // namespace

extern "C" stlcs_u32 stlcs_abi_version() {
    return ABI_VERSION;
}

extern "C" bool stlcs_start(Session* session, const HostApi* host,
                            const char* user_name) {
    if (!session || !host_api_valid(host) || !bounded_name_valid(user_name))
        return false;

    clear_bytes(reinterpret_cast<char*>(session), sizeof(Session));
    session->magic = SESSION_MAGIC;
    session->struct_size = sizeof(Session);
    session->host = host;
    session->state = SessionState::Locked;
    session->transition_start_ms = host->get_monotonic_ms(host->context);
    session->transition_alpha = 0u;
    session->transition_complete = 0u;
    if (session->transition_start_ms == MONOTONIC_UNAVAILABLE) {
        session->transition_alpha = 255u;
        session->transition_complete = 1u;
    }
    copy_text(session->user_name, USER_NAME_CAPACITY, user_name);
    copy_text(session->clock_text, CLOCK_CAPACITY, "--:--:--");
    copy_text(session->date_text, DATE_CAPACITY, "Loading date...");
    set_status(session, "Press ENTER to unlock");
    refresh_clock(session);
    return present(session);
}

extern "C" StepResult stlcs_step(Session* session) {
    if (!session || session->magic != SESSION_MAGIC ||
        session->struct_size < sizeof(Session) || !session->host ||
        session->state != SessionState::Locked)
        return StepResult::Error;

    InputEvent event{};
    for (stlcs_u32 count = 0u; count < MAX_INPUT_EVENTS_PER_STEP; ++count) {
        if (!session->host->poll_input(session->host->context, &event)) break;
        handle_input(session, event);
        if (session->state == SessionState::DesktopRequested) break;
    }

    if (session->state == SessionState::DesktopRequested)
        return StepResult::DesktopRequested;

    refresh_clock(session);
    if (!present(session)) {
        session->state = SessionState::Faulted;
        return StepResult::Error;
    }
    session->host->yield(session->host->context);
    return StepResult::Continue;
}

extern "C" void stlcs_stop(Session* session) {
    if (!session || session->magic != SESSION_MAGIC) return;
    clear_password(session);
    clear_bytes(reinterpret_cast<char*>(&session->last_presentation),
                sizeof(session->last_presentation));
    session->has_presentation = 0u;
    session->state = SessionState::Stopped;
    session->host = nullptr;
}

extern "C" stlcs_u32 stlcs_entry(const HostApi* host, Session* session,
                                 const char* user_name) {
    if (!stlcs_start(session, host, user_name)) return 1u;
    while (session->state == SessionState::Locked) {
        const StepResult result = stlcs_step(session);
        if (result == StepResult::Error) return 2u;
        if (result == StepResult::DesktopRequested) return 0u;
    }
    return session->state == SessionState::DesktopRequested ? 0u : 3u;
}


namespace {

/* Phase 2N.2: deliberately narrow CPL3 -> CPL0 service gate.
   RAX=service, RDI=version 3, RSI/RDX/RCX/R8/R9=bounded arguments.
   INT 0x81 is only the Lock Screen HostApi transport; it is not a general
   system-call ABI and does not enable EFER.SCE. */
static stlcs_u64 stlcs_host_service_call(stlcs_u64 service,
                                         stlcs_u64 a1 = 0u,
                                         stlcs_u64 a2 = 0u,
                                         stlcs_u64 a3 = 0u,
                                         stlcs_u64 a4 = 0u,
                                         stlcs_u64 a5 = 0u) {
    register stlcs_u64 rax asm("rax") = service;
    register stlcs_u64 rdi asm("rdi") = 3u;
    register stlcs_u64 rsi asm("rsi") = a1;
    register stlcs_u64 rdx asm("rdx") = a2;
    register stlcs_u64 rcx asm("rcx") = a3;
    register stlcs_u64 r8  asm("r8")  = a4;
    register stlcs_u64 r9  asm("r9")  = a5;
    asm volatile("int $0x81"
        : "+a"(rax)
        : "D"(rdi), "S"(rsi), "d"(rdx), "c"(rcx), "r"(r8), "r"(r9)
        : "r10", "r11", "memory", "cc");
    return rax;
}

static bool stlcs_host_poll_input(void*, starlix::stlcs::InputEvent* output) {
    if (!output) return false;
    return stlcs_host_service_call(1u,
        reinterpret_cast<stlcs_u64>(output)) == 1u;
}

static bool stlcs_host_get_clock(void*, char* clock_out,
                                  stlcs_u32 clock_capacity,
                                  char* date_out, stlcs_u32 date_capacity) {
    if (clock_out && clock_capacity) clock_out[0] = '\0';
    if (date_out && date_capacity) date_out[0] = '\0';
    /* A trusted RTC/time service is not part of the current kernel ABI. */
    return false;
}

static void stlcs_tile_rect(stlcs_u32* pixels,
                            stlcs_u32 tile_x, stlcs_u32 tile_y,
                            stlcs_u32 tile_width, stlcs_u32 tile_height,
                            stlcs_u32 x, stlcs_u32 y,
                            stlcs_u32 width, stlcs_u32 height,
                            stlcs_u32 color) {
    const stlcs_u32 left = x > tile_x ? x : tile_x;
    const stlcs_u32 top = y > tile_y ? y : tile_y;
    const stlcs_u32 right_edge = x + width;
    const stlcs_u32 bottom_edge = y + height;
    const stlcs_u32 tile_right = tile_x + tile_width;
    const stlcs_u32 tile_bottom = tile_y + tile_height;
    const stlcs_u32 right = right_edge < tile_right ? right_edge : tile_right;
    const stlcs_u32 bottom = bottom_edge < tile_bottom ? bottom_edge : tile_bottom;
    if (left >= right || top >= bottom) return;
    for (stlcs_u32 py = top; py < bottom; ++py)
        for (stlcs_u32 px = left; px < right; ++px)
            pixels[display_tile_pixel_index(py - tile_y, px - tile_x)] = color;
}

static stlcs_u32 stlcs_text_length(const char* text, stlcs_u32 limit) {
    stlcs_u32 length = 0u;
    while (text && length < limit && text[length] != '\0') ++length;
    return length;
}

static void stlcs_tile_text(stlcs_u32* pixels,
                            stlcs_u32 tile_x, stlcs_u32 tile_y,
                            stlcs_u32 tile_width, stlcs_u32 tile_height,
                            stlcs_u32 x, stlcs_u32 y,
                            const char* text, stlcs_u32 capacity,
                            stlcs_u32 color, stlcs_u32 scale) {
    if (!text || scale == 0u) return;
    const stlcs_u32 length = stlcs_text_length(text, capacity);
    for (stlcs_u32 index = 0u; index < length; ++index) {
        unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch < 32u || ch > 127u) ch = static_cast<unsigned char>('?');
        const unsigned char* glyph =
            starlix::stlcs::font::STLCS_FONT_8X8[ch - 32u];
        const stlcs_u32 origin_x = x + index * 8u * scale;
        for (stlcs_u32 row = 0u; row < 8u; ++row) {
            const unsigned char bits = glyph[row];
            for (stlcs_u32 column = 0u; column < 8u; ++column) {
                if ((bits & (1u << column)) == 0u) continue;
                stlcs_tile_rect(pixels, tile_x, tile_y, tile_width,
                    tile_height, origin_x + column * scale,
                    y + row * scale, scale, scale, color);
            }
        }
    }
}

static stlcs_u32 stlcs_center_text_x(const char* text, stlcs_u32 capacity,
                                      stlcs_u32 screen_width,
                                      stlcs_u32 scale) {
    const stlcs_u32 length = stlcs_text_length(text, capacity);
    const stlcs_u32 width = length * 8u * scale;
    return width < screen_width ? (screen_width - width) / 2u : 0u;
}

static bool stlcs_render_presentation(
    const starlix::stlcs::Presentation* presentation) {
    using namespace starlix::stlcs;
    if (!presentation) return false;
    DisplayInfo display{};
    if (stlcs_host_service_call(7u,
            reinterpret_cast<stlcs_u64>(&display)) != 1u ||
        display.struct_size != sizeof(DisplayInfo) ||
        display.width < 320u || display.width > 4096u ||
        display.height < 240u || display.height > 2160u ||
        display.pixels_per_scanline < display.width ||
        display.reserved != 0u ||
        (display.pixel_format != 1u && display.pixel_format != 2u))
        return false;

    constexpr stlcs_u32 TILE_WIDTH = DISPLAY_TILE_EDGE;
    constexpr stlcs_u32 TILE_HEIGHT = DISPLAY_TILE_EDGE;
    constexpr stlcs_u32 TILE_PIXELS = DISPLAY_TILE_MAX_PIXELS;
    static_assert(TILE_PIXELS * sizeof(stlcs_u32) <= 4096u,
                  "user display tile ABI exceeds the bounded tile budget");
    constexpr stlcs_u32 BACKGROUND = 0x000F172Au;
    constexpr stlcs_u32 PANEL = 0x001E293Bu;
    constexpr stlcs_u32 CYAN = 0x0038BDF8u;
    constexpr stlcs_u32 WHITE = 0x00F8FAFCu;
    constexpr stlcs_u32 MUTED = 0x0064748Bu;
    constexpr stlcs_u32 AMBER = 0x00FBBF24u;
    stlcs_u32 tile_pixels[TILE_PIXELS]{};

    const stlcs_u32 center_x = display.width / 2u;
    const stlcs_u32 center_y = display.height / 2u;
    const stlcs_u32 avatar_x = center_x > 32u ? center_x - 32u : 0u;
    const stlcs_u32 avatar_y = center_y > 90u ? center_y - 90u : 8u;
    const stlcs_u32 box_width = display.width < 440u
        ? display.width - 48u : 360u;
    const stlcs_u32 box_x = (display.width - box_width) / 2u;
    const stlcs_u32 box_y = center_y + 34u;
    const stlcs_u32 status_y = box_y + 58u;
    const stlcs_u32 title_y = display.height / 4u;
    const stlcs_u32 clock_y = title_y + 32u;

    for (stlcs_u32 tile_y = 0u; tile_y < display.height; tile_y += TILE_HEIGHT) {
        const stlcs_u32 tile_h = display.height - tile_y < TILE_HEIGHT
            ? display.height - tile_y : TILE_HEIGHT;
        for (stlcs_u32 tile_x = 0u; tile_x < display.width; tile_x += TILE_WIDTH) {
            const stlcs_u32 tile_w = display.width - tile_x < TILE_WIDTH
                ? display.width - tile_x : TILE_WIDTH;
            for (stlcs_u32 i = 0u; i < TILE_PIXELS; ++i)
                tile_pixels[i] = BACKGROUND;

            stlcs_tile_rect(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                0u, 0u, display.width, 3u, CYAN);
            const char title[] = "STARLIX OS";
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                stlcs_center_text_x(title, sizeof(title), display.width, 2u),
                title_y, title, sizeof(title), CYAN, 2u);
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                stlcs_center_text_x(presentation->clock_text, CLOCK_CAPACITY,
                                   display.width, 2u),
                clock_y, presentation->clock_text, CLOCK_CAPACITY, WHITE, 2u);
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                stlcs_center_text_x(presentation->date_text, DATE_CAPACITY,
                                   display.width, 1u),
                clock_y + 22u, presentation->date_text, DATE_CAPACITY, MUTED, 1u);

            stlcs_tile_rect(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                avatar_x - 2u, avatar_y - 2u, 68u, 68u, CYAN);
            stlcs_tile_rect(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                avatar_x, avatar_y, 64u, 64u, PANEL);
            const char initial[2] = {
                presentation->user_name[0] ? presentation->user_name[0] : 'S', '\0'
            };
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                avatar_x + 24u, avatar_y + 20u, initial, sizeof(initial), WHITE, 3u);
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                stlcs_center_text_x(presentation->user_name, USER_NAME_CAPACITY,
                                   display.width, 2u),
                avatar_y + 76u, presentation->user_name,
                USER_NAME_CAPACITY, WHITE, 2u);

            stlcs_tile_rect(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                box_x - 2u, box_y - 2u, box_width + 4u, 46u, CYAN);
            stlcs_tile_rect(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                box_x, box_y, box_width, 42u, PANEL);
            const char placeholder[] = "Enter password";
            const char* password_text = presentation->password_mask[0]
                ? presentation->password_mask : placeholder;
            const stlcs_u32 password_capacity = presentation->password_mask[0]
                ? PASSWORD_CAPACITY : sizeof(placeholder);
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                box_x + 12u, box_y + 15u, password_text,
                password_capacity, presentation->password_mask[0] ? WHITE : MUTED, 1u);
            stlcs_tile_text(tile_pixels, tile_x, tile_y, tile_w, tile_h,
                stlcs_center_text_x(presentation->status_text, STATUS_CAPACITY,
                                   display.width, 1u),
                status_y, presentation->status_text, STATUS_CAPACITY, AMBER, 1u);

            if (presentation->transition_alpha < 255u) {
                const stlcs_u32 alpha = presentation->transition_alpha;
                for (stlcs_u32 row = 0u; row < tile_h; ++row) {
                    for (stlcs_u32 column = 0u; column < tile_w; ++column) {
                        stlcs_u32& pixel = tile_pixels[
                            display_tile_pixel_index(row, column)];
                        const stlcs_u32 red = (((pixel >> 16u) & 0xFFu) * alpha + 127u) / 255u;
                        const stlcs_u32 green = (((pixel >> 8u) & 0xFFu) * alpha + 127u) / 255u;
                        const stlcs_u32 blue = ((pixel & 0xFFu) * alpha + 127u) / 255u;
                        pixel = (red << 16u) | (green << 8u) | blue;
                    }
                }
            }

            DisplayBlit request{};
            request.struct_size = sizeof(DisplayBlit);
            request.x = tile_x;
            request.y = tile_y;
            request.width = tile_w;
            request.height = tile_h;
            request.source_stride = TILE_WIDTH;
            request.pixels = reinterpret_cast<stlcs_u64>(tile_pixels);
            if (stlcs_host_service_call(8u,
                    reinterpret_cast<stlcs_u64>(&request)) != 1u)
                return false;
        }
    }
    return stlcs_host_service_call(9u) == 1u;
}

static bool stlcs_host_present(
    void*, const starlix::stlcs::Presentation* presentation) {
    return stlcs_render_presentation(presentation);
}

static starlix::stlcs::AuthResult stlcs_host_authenticate(
    void*, const char* user_name, const char* password,
    stlcs_u32 password_bytes) {
    return static_cast<starlix::stlcs::AuthResult>(stlcs_host_service_call(
        4u, reinterpret_cast<stlcs_u64>(user_name),
        reinterpret_cast<stlcs_u64>(password), password_bytes));
}

static bool stlcs_host_request_desktop(void*, const char* user_name) {
    return stlcs_host_service_call(5u,
        reinterpret_cast<stlcs_u64>(user_name)) == 1u;
}

static void stlcs_host_yield(void*) {
    (void)stlcs_host_service_call(6u);
}

static stlcs_u64 stlcs_host_get_monotonic_ms(void*) {
    return stlcs_host_service_call(10u);
}

} // namespace

/* The loader enters this no-argument function at CPL3. HostApi and Session are
   placed on the mapped user stack; no kernel function pointer is exposed to
   the component. Until trusted authentication/desktop services are present,
   stlcs_entry stays on the locked screen and cannot request desktop handoff. */
extern "C" [[noreturn]] void stlcs_bootstrap_main() {
    using namespace starlix::stlcs;
    HostApi host{};
    host.abi_version = ABI_VERSION;
    host.struct_size = sizeof(HostApi);
    host.context = nullptr;
    host.poll_input = stlcs_host_poll_input;
    host.get_clock = stlcs_host_get_clock;
    host.present = stlcs_host_present;
    host.authenticate = stlcs_host_authenticate;
    host.request_desktop = stlcs_host_request_desktop;
    host.yield = stlcs_host_yield;
    host.get_monotonic_ms = stlcs_host_get_monotonic_ms;

    Session session{};
    constexpr char user_name[] = "Starlix User";
    (void)stlcs_entry(&host, &session, user_name);

    /* Unexpected component return is still locked. Do not return through the
       initial IRETQ frame or launch a desktop implicitly. */
    if (session.magic == SESSION_MAGIC) {
        stlcs_stop(&session);
    }
    for (;;) stlcs_host_yield(nullptr);
}

/* IRETQ starts with a 16-byte-aligned RSP, not a normal CALL entry frame.
   This naked shim performs the first CALL with that alignment so the C++
   bootstrap receives the SysV x86-64 entry alignment (RSP mod 16 == 8). */
extern "C" [[noreturn]] __attribute__((naked)) void stlcs_bootstrap() {
    asm volatile(
        "call stlcs_bootstrap_main\n"
        "1: pause\n"
        "jmp 1b\n"
    );
}

} // namespace starlix::stlcs
