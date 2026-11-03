#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail() { echo "[FAIL] $1" >&2; exit 1; }
pass() { echo "[PASS] $1"; }

SPLASH="$ROOT/boot/x64/rxload.efi/src/boot_splash.hpp"
MAIN="$ROOT/kernel/src/main.cpp"
GUI="$ROOT/kernel/src/gui/window.cpp"
FB="$ROOT/kernel/src/drivers/framebuffer.cpp"
FB_POLICY="$ROOT/kernel/src/drivers/framebuffer_policy.hpp"
SYSTEM="$ROOT/kernel/src/loader/system_component.cpp"
SYSTEM_H="$ROOT/kernel/src/loader/system_component.hpp"
ABI="$ROOT/user_components/stlcs/stlcs_abi.hpp"
STLCS="$ROOT/user_components/stlcs/stlcs.cpp"
SERVICE="$ROOT/kernel/src/loader/user_host_service.cpp"
POLICY="$ROOT/kernel/src/loader/user_host_service_policy.hpp"

# Existing UEFI logo and spinner must remain on one shared alpha curve.
grep -q 'BootSplashDrawSpinner(s, fade);' "$SPLASH" || fail 'spinner does not use logo fade alpha'
grep -q '220u \* fadeAlpha' "$SPLASH" || fail 'spinner alpha is not coupled to the logo'
pass 'UEFI logo and spinner retain the common graphical fade'

# Kernel diagnostics remain serial-only. The framebuffer is reserved for the splash/Lock Screen.
if sed -n '/static void DebugLog(/,/^}/p' "$MAIN" | grep -Eq 'DrawString|FillRect|PutPixel|DrawText'; then
    fail 'kernel DebugLog paints text onto the graphical framebuffer'
fi
GUI_DRAWER="$(sed -n '/static void gui_draw_usb_input_status(/,/^}/p' "$GUI")"
if printf '%s\n' "$GUI_DRAWER" | grep -Eq 'DrawString|FillRect|PutPixel|DrawText'; then
    fail 'kernel USB/input diagnostic drawer paints the graphical framebuffer'
fi
grep -q 'serial::print(msg)' "$MAIN" || fail 'kernel DebugLog does not retain serial output'
pass 'kernel boot/debug text stays off-screen while serial diagnostics remain'

# The kernel owns the inherited UEFI splash fade and uses a real, initialized PIT.
grep -q 'bool fade_to_black(unsigned int duration_ms)' "$FB" || fail 'kernel framebuffer fade entry missing'
grep -q 'timer_is_ready()' "$FB" || fail 'kernel fade does not require a ready PIT timer'
grep -q 'interrupts_enabled()' "$FB" || fail 'kernel fade does not require interrupts enabled'
grep -q 'asm volatile("hlt"' "$FB" || fail 'kernel fade is not clock-paced'
grep -q 'fade_remaining(step, total_steps)' "$FB" || fail 'fade does not use monotonic remaining-alpha policy'
grep -q 'fade_scale_channel' "$FB" || fail 'fade does not progressively scale pixel channels'
if grep -q 'clear_black' "$FB"; then fail 'kernel fade has an instantaneous fake-black fallback'; fi
pass 'kernel performs PIT-clocked fade without fake delay or instantaneous fallback'

# Fade occurs after the component has been prepared and before the first CPL3 IRETQ.
FADE_POS="$(grep -n 'framebuffer::fade_to_black(660u)' "$SYSTEM" | cut -d: -f1)"
ENTRY_POS="$(grep -n 'StNtStartUserProcess(&handle)' "$SYSTEM" | cut -d: -f1)"
[[ -n "$FADE_POS" && -n "$ENTRY_POS" && "$FADE_POS" -lt "$ENTRY_POS" ]] ||
    fail 'kernel splash fade is not before first user-mode entry'
grep -q 'SplashFadeUnavailable' "$SYSTEM_H" || fail 'transition failure has no explicit startup result'
grep -q 'SPLASH_FADE_OUT=PASS' "$SYSTEM" || fail 'kernel transition success diagnostic missing'
grep -q 'StNtAbortUserProcess(&handle)' "$SYSTEM" || fail 'fade failure does not roll back the prepared process'
pass 'kernel splash fade runs before Ring-3 entry and fails closed with rollback'

# ABI 3 replaces the old privileged FadeScreen command with a read-only monotonic clock.
grep -q 'ABI_VERSION = 3u' "$ABI" || fail 'STLCS ABI version is not 3'
grep -q 'SERVICE_ABI_VERSION = 3ULL' "$POLICY" || fail 'HostApi service ABI version is not 3'
grep -q 'MonotonicMilliseconds = 10ULL' "$POLICY" || fail 'monotonic clock service ID missing'
grep -q 'Service::MonotonicMilliseconds' "$SERVICE" || fail 'monotonic service dispatch missing'
grep -q 'timer_now_ms()' "$SERVICE" || fail 'monotonic service does not read PIT clock'
grep -q 'rdi asm("rdi") = 3u' "$STLCS" || fail 'STLCS does not transmit service ABI version 3'
if grep -Eq 'FadeScreen|fade_screen|service_fade_screen' "$SERVICE" "$POLICY" "$ABI" "$STLCS"; then
    fail 'legacy user-callable screen-fade service remains'
fi
pass 'versioned HostApi ABI v3 exposes only the restricted monotonic clock'

# CPL3 transition is elapsed-time based and still uses the strict 32x32/4-KiB tile ABI.
grep -q 'get_monotonic_ms' "$ABI" || fail 'monotonic clock callback missing from HostApi'
grep -q 'lock_screen_fade_alpha' "$ABI" || fail 'shared Lock Screen elapsed-time alpha function missing'
grep -q 'transition_alpha' "$STLCS" || fail 'presentation does not carry transition alpha'
grep -q 'transition_start_ms' "$STLCS" || fail 'Lock Screen transition start time missing'
grep -q 'DISPLAY_TILE_EDGE = 32u' "$ABI" || fail '32x32 tile ABI was not preserved'
grep -q 'DISPLAY_TILE_MAX_PIXELS \* sizeof(stlcs_u32) <= 4096u' "$ABI" || fail '4-KiB HostApi stack budget missing'
grep -q 'TILE_WIDTH = DISPLAY_TILE_EDGE' "$STLCS" || fail 'renderer does not use shared 32-pixel tile width'
grep -q 'display_tile_pixel_index(row, column)' "$STLCS" || fail 'renderer does not use the shared tile index'
if grep -Eq '\* *64u|TILE_WIDTH *= *64u|TILE_HEIGHT *= *64u' "$STLCS"; then
    fail 'legacy 64x64 tile stride reintroduced'
fi
pass 'Lock Screen fades in from PIT elapsed time while retaining the Phase 2N.6 tile/stack ABI'

echo 'PHASE2P7_1_BOOT_TO_LOCK_TRANSITION: PASS'
