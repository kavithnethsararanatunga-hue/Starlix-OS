#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT/boot/x64/rxload.efi/src/main.cpp" "$ROOT/boot/x64/rxload.efi/src/boot_splash.hpp" <<'PYTEST'
from pathlib import Path
import sys
main = Path(sys.argv[1]).read_text()
splash = Path(sys.argv[2]).read_text()

helper_start = main.index("static bool FinalizeMemoryMapAndExitBootServices")
helper_end = main.index("extern \"C\" EFI_STATUS ABI efi_main", helper_start)
helper = main[helper_start:helper_end]
preflight = helper.index("bool capacityReady = false")
preflight_map = helper.index("preflightSize, memoryMap")
fade = helper.index("BootSplashFadeOutAndStop(BS,splashState)")
exit_loop = helper.index("bool attemptedExit = false")
final_map = helper.index("mapSize, memoryMap", exit_loop)
exit_call = helper.index("BS->ExitBootServices", exit_loop)
assert preflight < preflight_map < fade < exit_loop < final_map < exit_call
assert "AllocatePool" in helper[preflight:fade]
assert "FreePool" in helper[preflight:fade]
assert "BOOT_SPLASH_STATE* splashState" in helper
assert "BootSplashStopState(BS, splashState)" not in main[main.index("extern \"C\" EFI_STATUS ABI efi_main"):]

fade_start = splash.index("static bool BootSplashFadeOutAndStop")
brace = splash.index("{", fade_start)
depth = 0
fade_end = None
for i in range(brace, len(splash)):
    if splash[i] == "{": depth += 1
    elif splash[i] == "}":
        depth -= 1
        if depth == 0:
            fade_end = i + 1
            break
assert fade_end is not None
fade_body = splash[fade_start:fade_end]
assert "BootSplashStopTimer(bs,s)" in fade_body
assert "bs->Stall(kBootSplashExitFrameStallUs)" in fade_body
assert "kBootSplashExitFadeFrames" in fade_body
assert "(void)BootSplashPump(s)" in fade_body
assert "s->elapsedUs += kBootSplashFrameStepUs" in fade_body
assert "BootSplashRender(s)" in fade_body
assert "s->active=false" in fade_body

render_start = splash.index("static void BootSplashRender")
render_end = splash.index("static void ABI BootSplashTimerNotify", render_start)
render = splash[render_start:render_end]
assert "entranceFade * s->exitFadeAlpha" in render
assert "BootSplashDrawLogo(s,fade)" in render
assert "BootSplashDrawSpinner(s, fade)" in render
logo_start = splash.index("static void BootSplashDrawLogo")
logo_end = splash.index("static BOOT_SPLASH_COLOR BootSplashGradientColor", logo_start)
logo = splash[logo_start:logo_end]
assert "!fadeAlpha" not in logo.split("{", 1)[1].split("}", 1)[0], \
    "zero-alpha render must overwrite old logo pixels with black"

# Once ExitBootServices has been attempted, retries must remain map/exit-only.
post_attempt = helper[helper.index("bool attemptedExit = false"):]
assert "BS->GetMemoryMap" in post_attempt and "BS->ExitBootServices" in post_attempt
exit_guard = post_attempt.index("attemptedExit = true")
retry_tail = post_attempt[exit_guard:]
assert "AllocatePool" not in retry_tail and "FreePool" not in retry_tail
assert "for (;;) asm volatile(\"hlt\")" in retry_tail

print("RXLOAD_MEMORY_MAP_SPLASH_FADE: PASS")
print("  PASS: map capacity preflight completes before fade")
print("  PASS: logo and SVG spinner share terminal alpha")
print("  PASS: UEFI timer is closed before the final memory-map snapshot")
print("  PASS: final GetMemoryMap precedes ExitBootServices")
print("  PASS: invalid-key retry remains map/exit-only")
print("  PASS: pending animation frames are drained before terminal fade")
PYTEST
