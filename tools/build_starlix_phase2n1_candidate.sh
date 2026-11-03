#!/usr/bin/env bash
# Build the normal (non-diagnostic) production kernel and a disposable Phase 2N.1
# candidate whose RXFS fixture contains the real stlcs.ect component.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -gt 1 ]]; then
  echo "Usage: $0 [phase10-run-directory]" >&2
  exit 2
fi
PHASE10_RUN="${1:-$ROOT/build/candidate_rxfs_phase10/run_20261027_215640}"
[[ -d "$PHASE10_RUN" ]] || { echo "Phase 10 run directory missing: $PHASE10_RUN" >&2; exit 1; }
PHASE10_RUN="$(cd "$PHASE10_RUN" && pwd)"
[[ -s "$PHASE10_RUN/starlix-rxfs-phase10.img" ]] || {
  echo "Phase 10 image missing or empty: $PHASE10_RUN/starlix-rxfs-phase10.img" >&2; exit 1;
}
STLCS=""
for candidate in "$ROOT/RXFS/sys/User/stlcs.ect" "$ROOT/rxfs/sys/User/stlcs.ect"; do
  if [[ -s "$candidate" ]]; then STLCS="$candidate"; break; fi
done
[[ -n "$STLCS" ]] || { echo "RXFS/sys/User/stlcs.ect not found" >&2; exit 1; }
for cmd in clang++ nm readelf sha256sum python3; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 1; }
done
cd "$ROOT"
STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP="$ROOT/.backups/phase2n1_candidate_artifacts_$STAMP"
mkdir -p "$BACKUP"
for rel in kernel/build tools/ect/pack_kernel_ect \
  boot/x64/bootx64.efi/build boot/x64/stbootmngr.efi/build boot/x64/rxload.efi/build; do
  if [[ -e "$ROOT/$rel" ]]; then
    mkdir -p "$BACKUP/$(dirname "$rel")"
    cp -a "$ROOT/$rel" "$BACKUP/$rel"
  fi
done
echo "[BACKUP] Generated kernel/EFI artifacts: $BACKUP"

echo "[1/5] Building normal production kernel (Ring 3 smoke diagnostic disabled)..."
STARLIX_ENABLE_RING3_SMOKE_TEST=0 bash scripts/build_kernel.sh
ELF="$ROOT/kernel/build/kernel.elf"
BIN="$ROOT/kernel/build/kernel.bin"
[[ -s "$ELF" && -s "$BIN" ]] || { echo "Kernel build artifacts missing" >&2; exit 1; }
ENTRY="$(readelf -h "$ELF" | awk '/Entry point address:/ {print $4; exit}')"
[[ "$ENTRY" == "0x200000" ]] || { echo "Unexpected ELF entry: $ENTRY" >&2; exit 1; }
if nm "$ELF" | grep -q 'starlix_user_probe_exit_dispatch'; then
  echo "ABORT: normal production ELF contains the diagnostic exit dispatcher" >&2; exit 1
fi
if [[ -n "$(nm -u "$ELF")" ]]; then
  echo "ABORT: undefined kernel symbols" >&2; nm -u "$ELF" >&2; exit 1
fi
readelf -S "$ELF" | grep -q '\.debug_info' || {
  echo "ABORT: complete debug information is missing from kernel.elf" >&2; exit 1;
}
PACKER="$ROOT/tools/ect/pack_kernel_ect"
clang++ -std=c++26 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto \
  -I"$ROOT/tools/ect/include" "$ROOT/tools/ect/pack_kernel_ect.cpp" -o "$PACKER"
ECT="$ROOT/build/RxKernel1.0.phase2n1-production.ect"
"$PACKER" "$BIN" "$ECT" "$ENTRY"
python3 - "$BIN" "$ECT" <<'PY'
from pathlib import Path
import struct, sys, zlib
kernel = Path(sys.argv[1]).read_bytes()
ect = Path(sys.argv[2]).read_bytes()
if not (152 < len(ect) <= 4 * 1024 * 1024): raise SystemExit("ECT size outside loader limit")
if ect[:2] != b"Px" or struct.unpack_from("<H", ect, 80)[0] != 128:
    raise SystemExit("ECT header invalid")
if struct.unpack_from("<I", ect, 82)[0] != 1 or struct.unpack_from("<I", ect, 90)[0] != 0 or struct.unpack_from("<I", ect, 98)[0] != 0:
    raise SystemExit("kernel ECT section/import/relocation policy mismatch")
entry = struct.unpack_from("<Q", ect, 106)[0]
if entry != 0x200000: raise SystemExit("kernel ECT entry point mismatch")
name = ect[128:136].split(b"\0", 1)[0]
vaddr, size, offset, flags = struct.unpack_from("<IIII", ect, 136)
if name != b".text" or vaddr != 0x200000 or flags != 0x5 or size != len(kernel) or offset != 152:
    raise SystemExit("kernel ECT .text metadata mismatch")
if ect[offset:offset+size] != kernel or len(ect) != offset + size:
    raise SystemExit("kernel ECT payload mismatch")
if zlib.crc32(kernel) & 0xffffffff != struct.unpack_from("<I", ect, 122)[0]:
    raise SystemExit("kernel ECT CRC32 mismatch")
print(f"PASS: normal production kernel ECT validated ({len(ect)} bytes; entry={entry:#x})")
PY
{
  echo "Starlix OS 1.0 Beta - Phase 2N.1 normal production candidate"
  echo "ELF entry: $ENTRY"
  echo "kernel.elf bytes: $(stat -c %s "$ELF")"
  echo "kernel.bin bytes: $(stat -c %s "$BIN")"
  echo "kernel ECT bytes: $(stat -c %s "$ECT")"
  sha256sum "$ELF" "$BIN" "$ECT" "$STLCS"
} > "$ROOT/build/phase2n1-production-manifest.txt"

echo "[2/5] Running ATA policy and production ECT tests..."
bash tests/test_ata_pio_policy.sh
bash tests/test_phase2n1_stlcs_ect_validation.sh
bash tests/test_phase2n1_rxfs_stlcs_fixture.sh

echo "[3/5] Creating disposable Phase 2N.1 VMware candidate..."
LOG="$ROOT/build/phase2n1-candidate-$STAMP.log"
set +e
bash tools/build_stbootmngr_phase12_runtime_candidate.sh \
  "$PHASE10_RUN" "$ECT" "$STLCS" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e
if (( STATUS != 0 )); then
  echo "Candidate pipeline failed (exit $STATUS); log: $LOG" >&2
  exit "$STATUS"
fi
VMX="$(awk -F'VMX: ' '/^VMX: / {print $2}' "$LOG" | tail -n1)"
[[ -n "$VMX" && -s "$VMX" ]] || { echo "VMX path not found in candidate log" >&2; exit 1; }
echo "[4/5] Recording final manifest..."
sha256sum "$ELF" "$BIN" "$ECT" "$STLCS" "$VMX" > "$ROOT/build/phase2n1-final-sha256.txt"
echo "[5/5] Candidate ready for local VMware hardware verification."
echo "VMX: $VMX"
echo "Expected next result after applying Phase 2N.1: RXFS mounted, stlcs.ect located and ECT validated."
echo "Expected fail-closed stopping point: USER_HOST_SERVICE_BRIDGE_UNAVAILABLE."
echo "Phase 2N production Ring 3 execution is NOT claimed by this candidate build."
