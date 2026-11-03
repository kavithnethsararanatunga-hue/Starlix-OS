#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RXLOAD="$ROOT/boot/x64/rxload.efi"
SRC="$RXLOAD/src/main.cpp"
HEADER="$RXLOAD/src/boot_splash.hpp"
BUILD="$ROOT/scripts/build.sh"
STBMGR="$ROOT/boot/x64/stbootmngr.efi/src/main.cpp"
HANDOFF="$ROOT/boot/x64/stbootmngr.efi/src/stboot_handoff.hpp"
ASSET_BUILDER="$ROOT/tools/embed_boot_splash_assets.py"
PASS=0
check() {
  if eval "$1"; then
    echo "[PASS] $2"
    PASS=$((PASS + 1))
  else
    echo "[FAIL] $2" >&2
    exit 1
  fi
}

check "grep -Fq '#include \"boot_splash.hpp\"' '$SRC'" "RXLOAD includes the graphical splash renderer"
check "grep -Fq 'BootSplashStateMatchesGOP(splashState, gop)' '$SRC' && grep -Fq 'BootSplashInitialize(BS, gop)' '$SRC'" "RXLOAD reuses the manager splash or initializes a standalone fallback"
check "grep -Fq 'BootSplashFadeOutAndStop(BS,splashState)' '$SRC'" "Shared splash fades to black and closes its timer before final ExitBootServices"
check "! grep -Fq 'BootSplashWait(BS' '$SRC'" "No artificial fade delay is inserted before real loading"
check "! grep -Eq 'OutputString|SetCursorPosition|ClearScreen' '$SRC'" "RXLOAD has no direct UEFI text-console output"
check "grep -Fq '#define PrintAt(out, col, row, msg) ((void)0)' '$SRC' && grep -Fq '#define DrawProgressBar(out, percent) ((void)0)' '$SRC'" "Legacy status text and progress-bar callsites are compiled out"
check "grep -Fq 'CreateEvent' '$HEADER' && grep -Fq 'SetTimer' '$HEADER' && grep -Fq 'CloseEvent' '$HEADER'" "UEFI timer event has create, start, cancel and close lifecycle"
check "grep -Fq 'pendingFrames' '$HEADER' && grep -Fq 'BootSplashPump' '$HEADER'" "Timer and loader checkpoints share a reentrancy-guarded GOP frame pump"
check "grep -Fq 'kBootSplashTimerPeriod100ns = 166667' '$HEADER' && grep -Fq 'kBootSplashExitFadeFrames = 40u' '$HEADER'" "Splash frame pacing and exit fade target 60 Hz"

check "grep -Fq 'BootSplashStartTimer(BS, splashState)' '$SRC' || grep -Fq 'BootSplashStartTimer(bs, s)' '$HEADER'" "Inherited splash timer can be resumed without clearing the existing frame"
check "grep -Fq 'BootSplashMinU32(72u' '$HEADER'" "Spinner is slightly smaller for the refined splash layout"
check "grep -Fq 'BootSplashInitializeState(BS, gop, &gStbootBootSplash)' '$STBMGR'" "stbtmngr starts the splash before RXFS/APFS loading"
python3 - "$STBMGR" <<'PY_ORDER'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
start = text.index('BootSplashInitializeState(BS, gop, &gStbootBootSplash)')
filesystem = text.index('TryRxfsBootEfi(ImageHandle, BS)')
assert start < filesystem, 'splash must start before RXFS/APFS discovery'
assert 'StbootSerialLog(BS, splashStarted' in text
print('[PASS] Splash initialization precedes RXFS/APFS work; outcome is serial-only')
PY_ORDER
PASS=$((PASS + 1))
check "! grep -Fq 'OutputString' '$STBMGR' && grep -Fq 'StbootSerialLog' '$STBMGR'" "UEFI debug text is hidden while serial diagnostics remain available"
check "grep -Fq 'STBOOT_CHILD_LOAD_OPTIONS' '$HANDOFF' && grep -Fq 'SplashState' '$HANDOFF'" "Versioned child handoff can share the parent splash state"
check "grep -Fq 'childLoadedImage->LoadOptions = &childOptions' '$STBMGR' && grep -Fq 'SplashState = gStbootBootSplash.active' '$STBMGR'" "The same splash timer/state is passed across StartImage"
check "grep -Fq 'sizeof(STBOOT_FS_HANDOFF) == 64' '$HANDOFF' && grep -Fq 'sizeof(STBOOT_CHILD_LOAD_OPTIONS) == 80' '$HANDOFF'" "Existing 64-byte filesystem ABI remains stable with an 80-byte splash wrapper"
check "grep -Fq '*supplied = false;' '$SRC' && grep -Fq 'legacy SSP fallback' '$SRC'" "Splash-only legacy handoff does not masquerade as a filesystem handoff"
check "! grep -Fq 'BootSplashStopState' '$STBMGR' && grep -Fq 'static STBOOT_CHILD_LOAD_OPTIONS childOptions' '$STBMGR'" "Parent timer and handoff state stay alive while the child loader executes"
check "! grep -Eq 'Stall\(|BootSplashWait\(' '$SRC' && ! grep -Eq 'Stall\(' '$STBMGR'" "No artificial waits are added to the real loader path"
check "! grep -Fq 'for (UINTN percent = 60; percent <= 85' '$SRC'" "Fake timed progress loop is removed from kernel loading"
check "grep -Fq 'BootSplashParseSvg' '$HEADER' && grep -Fq 'stroke-dasharray' '$HEADER' && grep -Fq 'spinDurationMs' '$HEADER'" "Embedded SVG subset parses geometry and animation timings"
check "grep -Fq 'embed_boot_splash_assets.py' '$BUILD'" "EFI build regenerates C arrays from source PNG and SVG assets"
python3 - "$BUILD" <<'PY_ORDER'
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text()
asset = text.index('embed_boot_splash_assets.py')
manager = text.index('stbtmngr.efi (persistent graphical splash')
assert asset < manager, 'splash C arrays must exist before stbtmngr compiles the shared renderer'
print('[PASS] Shared splash assets are generated before stbtmngr compilation')
PY_ORDER
PASS=$((PASS + 1))
check "test -s '$RXLOAD/src/assets/Stlg.png' && test -s '$RXLOAD/src/assets/StarlixBootSpinner.svg'" "Both boot assets are present under rxload.efi/src/assets"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 "$ASSET_BUILDER" \
  "$RXLOAD/src/assets/Stlg.png" \
  "$RXLOAD/src/assets/StarlixBootSpinner.svg" \
  "$TMP/boot_splash_assets.hpp"
check "grep -Fq 'kStlgLogoWidth = 512u' '$TMP/boot_splash_assets.hpp' && grep -Fq 'kStarlixBootSpinnerSvg' '$TMP/boot_splash_assets.hpp'" "Asset generator emits compact logo RGBA and SVG C arrays"
check "grep -Fq 'xmlns=\"http://www.w3.org/2000/svg\"' '$TMP/boot_splash_assets.hpp'" "Embedded SVG namespace is normalized without modifying source asset"

for CXX in clang++ g++; do
  command -v "$CXX" >/dev/null
  echo "[$CXX] SVG parser + GOP pixel renderer ASan/UBSan"
  "$CXX" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT" -I"$ROOT/include" -I"$ROOT/boot/x64/stbootmngr.efi/src" -I"$ROOT/kernel/src" \
    "$ROOT/tests/boot_splash_svg_selftest.cpp" -o "$TMP/boot-splash-$CXX"
  "$TMP/boot-splash-$CXX"
done

echo "GRAPHICAL_BOOT_SPLASH_TESTS: PASS ($PASS source/build contracts plus Clang/GCC sanitizer renders)"
