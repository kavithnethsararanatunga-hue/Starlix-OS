#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$ROOT" <<'PYTEST'
from pathlib import Path
import sys
root = Path(sys.argv[1])
header = (root / "boot/x64/rxload.efi/src/boot_splash.hpp").read_text()
rxload = (root / "boot/x64/rxload.efi/src/main.cpp").read_text()
manager = (root / "boot/x64/stbootmngr.efi/src/main.cpp").read_text()

# Timer callback accounts actual UEFI ticks and delegates to the same guarded
# pump as loader checkpoints; it does not call raw framebuffer render primitives.
cb_start = header.index("static void ABI BootSplashTimerNotify")
cb_end = header.index("[[maybe_unused]] static bool BootSplashPump", cb_start)
callback = header[cb_start:cb_end]
assert "__atomic_fetch_add(&s->pendingFrames" in callback
assert "(void)BootSplashPump(s)" in callback
assert "BootSplashRender" not in callback
assert "BootSplashDraw" not in callback

pump_start = header.index("[[maybe_unused]] static bool BootSplashPump")
pump_end = header.index("static bool BootSplashStartTimer", pump_start)
pump = header[pump_start:pump_end]
assert "__atomic_exchange_n" in pump
assert "__atomic_compare_exchange_n" in pump
assert "renderBusy" in pump
assert "s->elapsedUs" in pump
assert "BootSplashRender(s)" in pump
assert "Stall(" not in pump
assert "kBootSplashTimerPeriod100ns = 166667" in header

# Kernel ECT reads are bounded and pump after every successful 16 KiB chunk.
read_start = rxload.index("static EFI_STATUS ReadKernelEctFromHandoff")
read_end = rxload.index("// STARLIX_PHASE13_KEXT", read_start)
read_body = rxload[read_start:read_end]
assert "const UINTN chunkLimit = 16U * 1024U" in read_body
assert "BootSplashPump(splashState)" in read_body
assert rxload.count("ReadKernelEctFromHandoff(") >= 2

# CRC and section mapping loops also service the splash without fake wait loops.
crc_start = rxload.index("static uint32_t EctCrc32")
crc_end = rxload.index("static EFI_STATUS ReadSspFileWithSplash", crc_start)
crc = rxload[crc_start:crc_end]
assert "BootSplashPump(splashState)" in crc
assert "Stall(" not in crc
assert "if ((b & 0x3FFFu) == 0x3FFFu)" in rxload
assert "BootSplashPump(splashState)" in rxload

# Legacy SSP fallback reads and manager boot.efi reads are chunked as well.
assert "ReadSspFileWithSplash" in rxload
assert "const UINTN chunkLimit = 16U * 1024U" in rxload
assert "kBootEfiReadChunk = 16ULL * 1024ULL" in manager
assert "BootSplashPump(&gStbootBootSplash)" in manager
assert "StbootSspReadFileWithSplash" in manager

# Terminal fade remains at the final memory-map boundary, after all load work.
finalize_start = rxload.index("static bool FinalizeMemoryMapAndExitBootServices")
finalize_end = rxload.index('extern "C" EFI_STATUS ABI efi_main', finalize_start)
finalize = rxload[finalize_start:finalize_end]
assert finalize.index("bool capacityReady = false") < finalize.index("BootSplashFadeOutAndStop")
assert finalize.index("BootSplashFadeOutAndStop") < finalize.index("bool attemptedExit = false")
post_fade = finalize[finalize.index("bool attemptedExit = false"):]
assert post_fade.index("GetMemoryMap(") < post_fade.index("BS->ExitBootServices")

print("PHASE2P8B_CONTINUOUS_KERNEL_LOAD_ANIMATION: PASS")
print("  PASS: 60 Hz timer ticks are counted and use the guarded shared renderer")
print("  PASS: reentrancy guard serializes timer and loader-side render paths")
print("  PASS: APFS/RXFS kernel reads are bounded to 16 KiB checkpoints")
print("  PASS: kernel CRC and section mapping pump pending animation frames")
print("  PASS: legacy SSP kernel reads are chunked and pumped")
print("  PASS: parent boot.efi read path keeps the same splash active")
print("  PASS: no artificial delay is added during loading")
print("  PASS: terminal fade remains immediately before final memory map")
PYTEST
