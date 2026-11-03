#!/usr/bin/env bash
# Build a Phase 2M Ring-3 diagnostic kernel and feed its ECT into the
# existing Phase 12 production-candidate pipeline. Phase 10 input is never edited.
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
  echo "Phase 10 RXFS disk image missing or empty." >&2; exit 1;
}
for cmd in clang++ ld.lld llvm-objcopy nm readelf python3 sha256sum; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 1; }
done

cd "$ROOT"
STAMP="$(date +%Y%m%d_%H%M%S)"
ARTIFACT_BACKUP="$ROOT/.backups/phase2m-phase12-artifacts-$STAMP"
mkdir -p "$ARTIFACT_BACKUP"
for rel in \
  kernel/build \
  build/RxKernel1.0.phase2m-ring3.ect \
  tools/ect/pack_kernel_ect \
  boot/x64/bootx64.efi/build \
  boot/x64/stbootmngr.efi/build \
  boot/x64/rxload.efi/build; do
  if [[ -e "$ROOT/$rel" ]]; then
    mkdir -p "$ARTIFACT_BACKUP/$(dirname "$rel")"
    cp -a "$ROOT/$rel" "$ARTIFACT_BACKUP/$rel"
  fi
done
echo "[BACKUP] Existing generated kernel/ECT/EFI outputs: $ARTIFACT_BACKUP"

echo "===== PHASE 2M RING-3 KERNEL BUILD ====="
STARLIX_ENABLE_RING3_SMOKE_TEST=1 bash scripts/build_kernel.sh
ELF="$ROOT/kernel/build/kernel.elf"
BIN="$ROOT/kernel/build/kernel.bin"
[[ -s "$ELF" && -s "$BIN" ]] || { echo "Kernel build outputs are missing." >&2; exit 1; }
ENTRY="$(readelf -h "$ELF" | awk '/Entry point address:/ {print $4; exit}')"
[[ "$ENTRY" == "0x200000" ]] || { echo "Unexpected kernel ELF entry: $ENTRY" >&2; exit 1; }
if ! nm "$ELF" | grep -q 'starlix_user_probe_exit_dispatch'; then
  echo "Opt-in Ring-3 diagnostic dispatcher was not linked into kernel.elf." >&2
  exit 1
fi
if [[ -n "$(nm -u "$ELF")" ]]; then
  echo "Kernel ELF contains undefined symbols; refusing to package." >&2
  nm -u "$ELF" >&2
  exit 1
fi

PACKER="$ROOT/tools/ect/pack_kernel_ect"
PACKER_SRC="$ROOT/tools/ect/pack_kernel_ect.cpp"
mkdir -p "$ROOT/build"
clang++ -std=c++26 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto \
  -I"$ROOT/tools/ect/include" "$PACKER_SRC" -o "$PACKER"
ECT="$ROOT/build/RxKernel1.0.phase2m-ring3.ect"
"$PACKER" "$BIN" "$ECT" "$ENTRY"

python3 - "$BIN" "$ECT" <<'PY'
from pathlib import Path
import struct, sys, zlib
kernel = Path(sys.argv[1]).read_bytes()
ect = Path(sys.argv[2]).read_bytes()
if not (152 < len(ect) <= 4 * 1024 * 1024):
    raise SystemExit(f"ABORT: ECT size outside Phase 12 limit: {len(ect)}")
if ect[:2] != b"Px":
    raise SystemExit("ABORT: ECT magic mismatch")
if struct.unpack_from("<H", ect, 80)[0] != 128:
    raise SystemExit("ABORT: ECT header size mismatch")
if struct.unpack_from("<I", ect, 82)[0] != 1:
    raise SystemExit("ABORT: expected exactly one .text section")
if struct.unpack_from("<I", ect, 90)[0] != 0 or struct.unpack_from("<I", ect, 98)[0] != 0:
    raise SystemExit("ABORT: kernel ECT unexpectedly has imports or relocations")
entry = struct.unpack_from("<Q", ect, 106)[0]
if entry != 0x200000:
    raise SystemExit(f"ABORT: ECT entry point mismatch: {entry:#x}")
name = ect[128:136].split(b"\0", 1)[0]
vaddr, raw_size, raw_off, flags = struct.unpack_from("<IIII", ect, 136)
if name != b".text" or vaddr != 0x200000 or flags != 0x5:
    raise SystemExit("ABORT: .text section metadata mismatch")
if raw_size != len(kernel) or raw_off != 152 or ect[raw_off:raw_off+raw_size] != kernel:
    raise SystemExit("ABORT: ECT payload does not exactly match kernel.bin")
if len(ect) != raw_off + raw_size:
    raise SystemExit("ABORT: ECT contains unexpected trailing or missing bytes")
crc = struct.unpack_from("<I", ect, 122)[0]
if zlib.crc32(kernel) & 0xffffffff != crc:
    raise SystemExit("ABORT: ECT payload CRC32 mismatch")
print(f"PASS: Phase 2M ECT validated ({len(ect)} bytes; entry={entry:#x}; CRC32; exact kernel payload)")
PY

MANIFEST="$ROOT/build/phase2m-ring3-kernel-manifest.txt"
{
  echo "Phase 2M Ring-3 diagnostic kernel"
  echo "ELF entry: $ENTRY"
  echo "kernel.bin bytes: $(stat -c %s "$BIN")"
  echo "ECT bytes: $(stat -c %s "$ECT")"
  sha256sum "$ELF" "$BIN" "$ECT"
} > "$MANIFEST"
cat "$MANIFEST"

echo "===== PHASE 12 CANDIDATE: PHASE 2M ECT ====="
LOG="$ROOT/build/phase2m-phase12-candidate-$(date +%Y%m%d_%H%M%S).log"
set +e
bash tools/build_stbootmngr_phase12_runtime_candidate.sh "$PHASE10_RUN" "$ECT" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e
if (( STATUS != 0 )); then
  echo "Phase 12 candidate build failed (exit $STATUS). Log: $LOG" >&2
  exit "$STATUS"
fi
VMX="$(awk -F'VMX: ' '/^VMX: / {print $2}' "$LOG" | tail -n1)"
[[ -n "$VMX" && -s "$VMX" ]] || { echo "Candidate VMX not found in build log: $LOG" >&2; exit 1; }
echo "[PASS] Phase 2M ECT candidate created: $VMX"
echo "[INFO] Phase 10 disk image was used as input; it was not modified."
echo "[INFO] This is the Phase 2H diagnostic payload in a Phase 2M kernel build, not stlcs.ect login launch."
