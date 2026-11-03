// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

using stlcs_u8  = unsigned char;
using stlcs_u16 = unsigned short;
using stlcs_u32 = unsigned int;
using stlcs_u64 = unsigned long long;
using stlcs_i32 = signed int;

namespace starlix::stlcs {

constexpr stlcs_u32 ABI_VERSION = 3u;
constexpr stlcs_u64 MONOTONIC_UNAVAILABLE = ~0ULL;
constexpr stlcs_u32 LOCK_SCREEN_FADE_DURATION_MS = 660u;
constexpr stlcs_u32 LOCK_SCREEN_FADE_FRAME_MS = 33u;
constexpr stlcs_u32 LOCK_SCREEN_FADE_STEPS =
    LOCK_SCREEN_FADE_DURATION_MS / LOCK_SCREEN_FADE_FRAME_MS;

constexpr stlcs_u32 lock_screen_fade_alpha(stlcs_u64 elapsed_ms) {
    const stlcs_u64 slot = elapsed_ms / LOCK_SCREEN_FADE_FRAME_MS;
    if (slot >= LOCK_SCREEN_FADE_STEPS) return 255u;
    return static_cast<stlcs_u32>((slot * 255ULL) / LOCK_SCREEN_FADE_STEPS);
}
constexpr stlcs_u32 SESSION_MAGIC = 0x534C4353u; // "SLCS"
constexpr stlcs_u32 USER_NAME_CAPACITY = 32u;
constexpr stlcs_u32 PASSWORD_CAPACITY = 64u;
constexpr stlcs_u32 STATUS_CAPACITY = 80u;
constexpr stlcs_u32 CLOCK_CAPACITY = 9u;
constexpr stlcs_u32 DATE_CAPACITY = 24u;

/* Keep HostApi display tiles small enough for the guarded 16 KiB kernel stack. */
constexpr stlcs_u32 DISPLAY_TILE_EDGE = 32u;
constexpr stlcs_u32 DISPLAY_TILE_MAX_PIXELS =
    DISPLAY_TILE_EDGE * DISPLAY_TILE_EDGE;

/* DisplayBlit always carries a tightly packed tile with the ABI edge as its
   row stride, including smaller edge tiles at the right/bottom of the screen. */
constexpr stlcs_u32 display_tile_pixel_index(stlcs_u32 row,
                                              stlcs_u32 column) {
    return row * DISPLAY_TILE_EDGE + column;
}

static_assert(DISPLAY_TILE_MAX_PIXELS * sizeof(stlcs_u32) <= 4096u,
              "DisplayBlit scratch must remain within a 4 KiB kernel-stack budget");

enum class InputKind : stlcs_u32 {
    None = 0u,
    KeyDown = 1u,
};

enum class KeyCode : stlcs_u32 {
    Character = 0u, // codepoint contains the printable ASCII character
    Enter = 1u,
    Backspace = 2u,
    Escape = 3u,
};

enum InputModifiers : stlcs_u32 {
    ModifierCtrl  = 1u << 0,
    ModifierAlt   = 1u << 1,
    ModifierMeta  = 1u << 2,
};

enum class AuthResult : stlcs_u32 {
    Accepted = 0u,
    Rejected = 1u,
    ServiceUnavailable = 2u,
};

enum class SessionState : stlcs_u32 {
    Empty = 0u,
    Locked = 1u,
    DesktopRequested = 2u,
    Stopped = 3u,
    Faulted = 4u,
};

enum class StepResult : stlcs_i32 {
    Error = -1,
    Continue = 0,
    DesktopRequested = 1,
};

struct InputEvent {
    InputKind kind;
    KeyCode key;
    stlcs_u32 codepoint;
    stlcs_u32 modifiers;
};

// Presentation contains only the masked password; the raw secret never crosses
// the display service boundary or enters the render cache.
struct Presentation {
    stlcs_u32 struct_size;
    stlcs_u32 state;
    char user_name[USER_NAME_CAPACITY];
    char clock_text[CLOCK_CAPACITY];
    char date_text[DATE_CAPACITY];
    char password_mask[PASSWORD_CAPACITY];
    char status_text[STATUS_CAPACITY];
    stlcs_u32 transition_alpha; // 0=black, 255=fully visible
};

/* Read-only display description and bounded pixel-tile request. All drawing
   decisions stay in the CPL3 component; the kernel service only validates and
   copies a tile to the UEFI GOP framebuffer. */
struct DisplayInfo {
    stlcs_u32 struct_size;
    stlcs_u32 width;
    stlcs_u32 height;
    stlcs_u32 pixels_per_scanline;
    stlcs_u32 pixel_format;
    stlcs_u32 reserved;
};

struct DisplayBlit {
    stlcs_u32 struct_size;
    stlcs_u32 x;
    stlcs_u32 y;
    stlcs_u32 width;
    stlcs_u32 height;
    stlcs_u32 source_stride;
    stlcs_u64 pixels;
    stlcs_u64 reserved;
};

static_assert(sizeof(DisplayInfo) == 24u);
static_assert(sizeof(DisplayBlit) == 40u);

// stlgn.ect supplies this table. authenticate() must forward credentials to
// lsass.ect (or its protected authentication endpoint), not authenticate in
// this UI component. It must not retain password_utf8 after returning.
struct HostApi {
    stlcs_u32 abi_version;
    stlcs_u32 struct_size;
    void* context;
    bool (*poll_input)(void* context, InputEvent* output);
    bool (*get_clock)(void* context, char* clock_out, stlcs_u32 clock_capacity,
                      char* date_out, stlcs_u32 date_capacity);
    bool (*present)(void* context, const Presentation* presentation);
    AuthResult (*authenticate)(void* context, const char* user_name,
                               const char* password_utf8, stlcs_u32 password_bytes);
    bool (*request_desktop)(void* context, const char* user_name);
    void (*yield)(void* context);
    stlcs_u64 (*get_monotonic_ms)(void* context);
};

static_assert(sizeof(HostApi) == 72u);

struct Session {
    stlcs_u32 magic;
    stlcs_u32 struct_size;
    const HostApi* host;
    SessionState state;
    stlcs_u32 password_length;
    char user_name[USER_NAME_CAPACITY];
    char password[PASSWORD_CAPACITY];
    char status_text[STATUS_CAPACITY];
    char clock_text[CLOCK_CAPACITY];
    char date_text[DATE_CAPACITY];
    Presentation last_presentation;
    stlcs_u32 has_presentation;
    stlcs_u64 transition_start_ms;
    stlcs_u32 transition_alpha;
    stlcs_u32 transition_complete;
};

// Public C ABI called by stlgn.ect / the user-component loader.
extern "C" stlcs_u32 stlcs_abi_version();
extern "C" bool stlcs_start(Session* session, const HostApi* host,
                            const char* user_name);
extern "C" StepResult stlcs_step(Session* session);
extern "C" void stlcs_stop(Session* session);
extern "C" stlcs_u32 stlcs_entry(const HostApi* host, Session* session,
                                 const char* user_name);

} // namespace starlix::stlcs
