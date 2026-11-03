#!/usr/bin/env bash
# Build user-mode stlcs.ect, the normal production kernel, and a disposable
# Phase 2N.2 VMware candidate containing the component in the RXFS fixture.
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
for cmd in clang++ nm readelf sha256sum python3 stat; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 1; }
done
cd "$ROOT"

STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP="$ROOT/.backups/phase2n2_candidate_artifacts_$STAMP"
mkdir -p "$BACKUP"
for rel in kernel/build tools/ect/pack_kernel_ect \
  boot/x64/bootx64.efi/build boot/x64/stbootmngr.efi/build boot/x64/rxload.efi/build; do
  if [[ -e "$ROOT/$rel" ]]; then
    mkdir -p "$BACKUP/$(dirname "$rel")"
    cp -a "$ROOT/$rel" "$BACKUP/$rel"
  fi
done
echo "[BACKUP] Generated artifacts saved: $BACKUP"

echo "[1/6] Building the production Lock Screen user component..."
bash scripts/build_stlcs.sh
STLCS="$ROOT/RXFS/sys/User/stlcs.ect"
[[ -s "$STLCS" ]] || { echo "stlcs.ect was not generated" >&2; exit 1; }

echo "[2/6] Running Phase 2N.2 and RXFS/ECT tests..."
bash tests/test_stlcs.sh
bash tests/test_phase2n2_host_service_policy.sh
bash tests/test_phase2n2_host_service_contract.sh
bash tests/test_phase2n2_framebuffer_policy.sh
bash tests/test_phase2n1_stlcs_ect_validation.sh
bash tests/test_phase2n1_rxfs_stlcs_fixture.sh
bash tests/test_rxfs_native.sh

echo "[3/6] Building normal production kernel..."
STARLIX_ENABLE_RING3_SMOKE_TEST=0 bash scripts/build_kernel.sh
NORMAL_ELF="$ROOT/kernel/build/kernel.elf"
NORMAL_BIN="$ROOT/kernel/build/kernel.bin"
[[ -s "$NORMAL_ELF" && -s "$NORMAL_BIN" ]] || { echo "Normal kernel outputs missing" >&2; exit 1; }
NORMAL_ENTRY="$(readelf -h "$NORMAL_ELF" | awk '/Entry point address:/ {print $4; exit}')"
[[ "$NORMAL_ENTRY" == "0x200000" ]] || { echo "Unexpected normal ELF entry $NORMAL_ENTRY" >&2; exit 1; }
[[ -z "$(nm -u "$NORMAL_ELF")" ]] || { echo "Normal kernel has undefined symbols" >&2; nm -u "$NORMAL_ELF" >&2; exit 1; }
if nm "$NORMAL_ELF" | grep -q 'starlix_user_probe_exit_dispatch'; then
  echo "Normal kernel unexpectedly contains test-only INT 0x80 dispatcher" >&2; exit 1
fi
readelf -S "$NORMAL_ELF" | grep -q '\.debug_info' || { echo "Normal kernel DWARF missing" >&2; exit 1; }
mkdir -p "$ROOT/build/phase2n2_artifacts/normal"
cp "$ROOT/kernel/build/kernel.elf" "$ROOT/kernel/build/kernel.bin" \
   "$ROOT/kernel/build/kernel.map" "$ROOT/kernel/build/kernel.symbols.txt" \
   "$ROOT/kernel/build/kernel.sections.txt" "$ROOT/build/phase2n2_artifacts/normal/"

 echo "[4/6] Building explicit Phase 2M diagnostic regression kernel..."
STARLIX_ENABLE_RING3_SMOKE_TEST=1 bash scripts/build_kernel.sh
DIAG_ELF="$ROOT/kernel/build/kernel.elf"
[[ -z "$(nm -u "$DIAG_ELF")" ]] || { echo "Diagnostic kernel has undefined symbols" >&2; nm -u "$DIAG_ELF" >&2; exit 1; }
nm "$DIAG_ELF" | grep -q 'starlix_user_probe_exit_dispatch' || {
  echo "Diagnostic kernel is missing test-only INT 0x80 dispatcher" >&2; exit 1;
}
mkdir -p "$ROOT/build/phase2n2_artifacts/diagnostic"
cp "$ROOT/kernel/build/kernel.elf" "$ROOT/kernel/build/kernel.bin" \
   "$ROOT/kernel/build/kernel.map" "$ROOT/kernel/build/kernel.symbols.txt" \
   "$ROOT/kernel/build/kernel.sections.txt" "$ROOT/build/phase2n2_artifacts/diagnostic/"

# Restore normal production artifacts as the default kernel/build state.
echo "[5/6] Restoring normal production kernel and packaging its ECT..."
STARLIX_ENABLE_RING3_SMOKE_TEST=0 bash scripts/build_kernel.sh
ELF="$ROOT/kernel/build/kernel.elf"
BIN="$ROOT/kernel/build/kernel.bin"
ENTRY="$(readelf -h "$ELF" | awk '/Entry point address:/ {print $4; exit}')"
PACKER="$ROOT/tools/ect/pack_kernel_ect"
clang++ -std=c++26 -O0 -g3 -fno-inline -fno-omit-frame-pointer -fno-lto \
  -I"$ROOT/tools/ect/include" "$ROOT/tools/ect/pack_kernel_ect.cpp" -o "$PACKER"
KERNEL_ECT="$ROOT/build/RxKernel1.0.phase2n2-production.ect"
"$PACKER" "$BIN" "$KERNEL_ECT" "$ENTRY"
python3 - "$BIN" "$KERNEL_ECT" <<'PY'
from pathlib import Path
import struct, sys, zlib
kernel=Path(sys.argv[1]).read_bytes(); ect=Path(sys.argv[2]).read_bytes()
if not (152 < len(ect) <= 4*1024*1024): raise SystemExit('kernel ECT size policy')
if ect[:2] != b'Px' or struct.unpack_from('<H',ect,80)[0] != 128: raise SystemExit('kernel ECT header')
if struct.unpack_from('<I',ect,82)[0] != 1 or struct.unpack_from('<I',ect,90)[0] != 0 or struct.unpack_from('<I',ect,98)[0] != 0: raise SystemExit('kernel ECT section/import/relocation policy')
entry=struct.unpack_from('<Q',ect,106)[0]
name=ect[128:136].split(b'\0',1)[0]
vaddr,size,offset,flags=struct.unpack_from('<IIII',ect,136)
if entry != 0x200000 or name != b'.text' or vaddr != 0x200000 or flags != 0x5 or size != len(kernel) or offset != 152: raise SystemExit('kernel ECT .text metadata')
if ect[offset:offset+size] != kernel or len(ect) != offset+size: raise SystemExit('kernel ECT payload mismatch')
if zlib.crc32(kernel)&0xffffffff != struct.unpack_from('<I',ect,122)[0]: raise SystemExit('kernel ECT CRC32')
print(f'PASS: Phase 2N.2 kernel ECT validated ({len(ect)} bytes; entry={entry:#x})')
PY

MANIFEST="$ROOT/build/phase2n2-production-manifest.txt"
{
  echo "Starlix OS 1.0 Beta - Phase 2N.2 production lock-screen candidate"
  echo "Normal ELF entry: $ENTRY"
  echo "Normal kernel.elf bytes: $(stat -c %s "$ELF")"
  echo "Normal kernel.bin bytes: $(stat -c %s "$BIN")"
  echo "Kernel ECT bytes: $(stat -c %s "$KERNEL_ECT")"
  echo "stlcs.ect bytes: $(stat -c %s "$STLCS")"
  sha256sum "$ELF" "$BIN" "$KERNEL_ECT" "$STLCS"
} > "$MANIFEST"

echo "[6/6] Creating disposable Phase 2N.2 VMware candidate..."
LOG="$ROOT/build/phase2n2-candidate-$STAMP.log"
set +e
bash tools/build_stbootmngr_phase12_runtime_candidate.sh \
  "$PHASE10_RUN" "$KERNEL_ECT" "$STLCS" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e
if (( STATUS != 0 )); then echo "Candidate build failed ($STATUS); log: $LOG" >&2; exit "$STATUS"; fi
VMX="$(awk -F'VMX: ' '/^VMX: / {print $2}' "$LOG" | tail -n1)"
[[ -n "$VMX" && -s "$VMX" ]] || { echo "Final VMX not found" >&2; exit 1; }
sha256sum "$ELF" "$BIN" "$KERNEL_ECT" "$STLCS" "$VMX" > "$ROOT/build/phase2n2-final-sha256.txt"
echo
echo "PHASE 2N.2 PRODUCTION LOCK-SCREEN CANDIDATE CREATED"
echo "VMX: $VMX"
echo "Expected first-entry evidence (VMware required): [RING3] STLCS_ENTRY=EXECUTING"
echo "Expected frame evidence (VMware required): [LOCKSCREEN] COMPONENT_ACTIVE=PASS"
echo "Authentication is deliberately unavailable; desktop handoff remains denied."
echo "Phase 2N production unlock is NOT claimed."
