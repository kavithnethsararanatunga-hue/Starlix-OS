#!/usr/bin/env bash
# Build a disposable VM candidate with actual boot.efi and a controlled ECT test payload.
# Large working data is kept under the configurable project-drive work base; only complete deliverables are published.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: $0 <phase10-run-directory> [production-kernel.ect] [stlcs.ect]" >&2
  exit 2
fi
PHASE13E_KEXT_BUNDLE="${STARLIX_PHASE13E_KEXT_BUNDLE:-}"
PHASE12_WORK_BASE="${STARLIX_PHASE12_WORK_BASE:-$ROOT/build/.phase12-work}"
KEXT_BUNDLE_NAME=""
KEXT_EXECUTABLE=""
KEXT_INFO_SOURCE=""
KEXT_SYS_SOURCE=""
if [[ -n "$PHASE13E_KEXT_BUNDLE" ]]; then
  [[ -d "$PHASE13E_KEXT_BUNDLE" ]] || {
    echo "Phase 13E KEXT bundle directory missing: $PHASE13E_KEXT_BUNDLE" >&2; exit 1;
  }
  PHASE13E_KEXT_BUNDLE="$(cd "$PHASE13E_KEXT_BUNDLE" && pwd)"
  KEXT_BUNDLE_NAME="$(basename "$PHASE13E_KEXT_BUNDLE")"
  KEXT_INFO_SOURCE="$PHASE13E_KEXT_BUNDLE/Contents/Info.plist"
  [[ -s "$KEXT_INFO_SOURCE" ]] || { echo "KEXT Info.plist missing" >&2; exit 1; }
  KEXT_EXECUTABLE="$(python3 - "$KEXT_INFO_SOURCE" <<'PYPLIST'
import plistlib, sys
from pathlib import Path
p = Path(sys.argv[1])
d = plistlib.loads(p.read_bytes())
expected = {
    "CFBundleIdentifier": "com.starlix.driver.intlgpudvs",
    "StarlixExecutable": "intlgpudvs.sys",
    "StarlixPCIVendorID": "0x8086",
    "StarlixPCIDeviceID": "0x0152",
    "StarlixNativeAcceleration": "false",
}
for k, v in expected.items():
    if d.get(k) != v:
        raise SystemExit(f"ABORT: unexpected {k}: {d.get(k)!r}")
print(d["StarlixExecutable"])
PYPLIST
)"
  [[ "$KEXT_BUNDLE_NAME" == "intlgpudvs.kext" && "$KEXT_EXECUTABLE" == "intlgpudvs.sys" ]] || {
    echo "ABORT: Phase 13E expects the intlgpudvs.kext/intlgpudvs.sys bundle." >&2; exit 1;
  }
  KEXT_SYS_SOURCE="$PHASE13E_KEXT_BUNDLE/Contents/Starlix/$KEXT_EXECUTABLE"
  [[ -s "$KEXT_SYS_SOURCE" ]] || { echo "KEXT SYS package missing" >&2; exit 1; }
  DUPLICATE_SYS="$(dirname "$PHASE13E_KEXT_BUNDLE")/$KEXT_EXECUTABLE"
  [[ -f "$DUPLICATE_SYS" ]] && cmp -s "$KEXT_SYS_SOURCE" "$DUPLICATE_SYS" || {
    echo "ABORT: KEXT SYS copy differs from bundle payload" >&2; exit 1;
  }
  [[ "$(stat -c %s "$KEXT_SYS_SOURCE")" -eq 21304 ]] || {
    echo "ABORT: unexpected Intel SYS package size" >&2; exit 1;
  }
  echo "[Phase 13E] Intel KEXT source metadata and duplicate SYS identity PASS"
fi
PHASE10_RUN="$(cd "$1" && pwd)"
PRODUCTION_ECT=""
STLCS_COMPONENT=""
if [[ $# -ge 2 ]]; then
  [[ -f "$2" ]] || { echo "Production ECT not found: $2" >&2; exit 1; }
  PRODUCTION_ECT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
fi
if [[ $# -ge 3 ]]; then
  [[ -f "$3" ]] || { echo "stlcs.ect not found: $3" >&2; exit 1; }
  STLCS_COMPONENT="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
elif [[ -n "$PRODUCTION_ECT" ]]; then
  for candidate in "$ROOT/RXFS/sys/User/stlcs.ect" "$ROOT/rxfs/sys/User/stlcs.ect"; do
    if [[ -f "$candidate" ]]; then
      STLCS_COMPONENT="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"
      break
    fi
  done
  [[ -n "$STLCS_COMPONENT" ]] || {
    echo "Production candidate requires the real RXFS/sys/User/stlcs.ect component." >&2
    exit 1
  }
fi
if [[ -n "$PHASE13E_KEXT_BUNDLE" && -z "$PRODUCTION_ECT" ]]; then
  echo "ABORT: Phase 13E VMware KEXT dry-run requires the production kernel ECT." >&2
  exit 1
fi
PHASE10_DISK="$PHASE10_RUN/starlix-rxfs-phase10.img"
for f in "$PHASE10_DISK" "$ROOT/scripts/build.sh" \
         "$ROOT/tools/apply_stbootmngr_phase12_runtime_fixture.py"; do
  [[ -f "$f" ]] || { echo "Required file missing: $f" >&2; exit 1; }
done
for cmd in clang clang++ lld-link sgdisk mcopy mmd dd file sha256sum cmp awk python3 stat; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 1; }
done
if [[ -n "$PRODUCTION_ECT" ]]; then
  ECT_BYTES="$(stat -c %s "$PRODUCTION_ECT")"
  if (( ECT_BYTES <= 152 || ECT_BYTES > 4 * 1024 * 1024 )); then
    echo "ABORT: production ECT must be >152 bytes and <=4 MiB." >&2
    exit 1
  fi
fi
if [[ -n "$STLCS_COMPONENT" ]]; then
  STLCS_BYTES="$(stat -c %s "$STLCS_COMPONENT")"
  if (( STLCS_BYTES <= 152 || STLCS_BYTES > 16384 )); then
    echo "ABORT: RXFS fixture currently supports stlcs.ect sizes >152 and <=16384 bytes." >&2
    exit 1
  fi
  [[ "$(od -An -tx1 -N2 "$STLCS_COMPONENT" | tr -d ' \n')" == "5078" ]] || {
    echo "ABORT: stlcs.ect does not begin with the ECT Px signature." >&2; exit 1;
  }
fi

echo "[Phase 12] Applying conditional host-fixture integration if needed..."
python3 "$ROOT/tools/apply_stbootmngr_phase12_runtime_fixture.py"

BASE="$ROOT/build/candidate_rxfs_phase12"
mkdir -p "$BASE"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_RUN="$BASE/run_$STAMP"
mkdir "$OUT_RUN"
mkdir -p "$PHASE12_WORK_BASE"
PHASE12_WORK_BASE="$(cd "$PHASE12_WORK_BASE" && pwd)"
WORK_ROOT="$(mktemp -d "$PHASE12_WORK_BASE/starlix-phase12.XXXXXX")"
RUN="$WORK_ROOT/run_$STAMP"
mkdir "$RUN"
mkdir -p "$WORK_ROOT/tmp"
export TMPDIR="$WORK_ROOT/tmp"
echo "[Phase 12] WSL workspace: $RUN"
echo "[Phase 12] Temporary files: $TMPDIR"
echo "[Phase 12] Final output: $OUT_RUN"

echo "[1/8] Building EFI images..."
bash "$ROOT/scripts/build.sh"
BOOT_EFI="$ROOT/boot/x64/rxload.efi/build/boot.efi"
[[ -s "$BOOT_EFI" ]] || { echo "Missing built boot.efi" >&2; exit 1; }
file "$BOOT_EFI"

if [[ -n "$PRODUCTION_ECT" ]]; then
  ECT_NAME="phase12-production-kernel.ect"
  ECT="$RUN/$ECT_NAME"
  echo "[2/8] Staging production ECT payload..."
  cp -- "$PRODUCTION_ECT" "$ECT"
else
  ECT_NAME="phase12-test-kernel.ect"
  ECT="$RUN/$ECT_NAME"
  echo "[2/8] Creating controlled ECT test payload..."
  python3 "$ROOT/tools/build_stbootmngr_phase12_test_ect.py" "$ECT"
fi

if [[ -n "$PHASE13E_KEXT_BUNDLE" ]]; then
  echo "[3/8] Staging the real Intel KEXT SYS package and Info.plist..."
  DB_SYS_NAME="$KEXT_EXECUTABLE"
  KEXT_INFO_NAME="intlgpudvs.Info.plist"
  DB_SYS="$RUN/$DB_SYS_NAME"
  KEXT_INFO_COPY="$RUN/$KEXT_INFO_NAME"
  cp -- "$KEXT_SYS_SOURCE" "$DB_SYS"
  cp -- "$KEXT_INFO_SOURCE" "$KEXT_INFO_COPY"
  cmp "$KEXT_SYS_SOURCE" "$DB_SYS"
  cmp "$KEXT_INFO_SOURCE" "$KEXT_INFO_COPY"
  echo "PASS: Intel KEXT source copies are byte-identical"
else
  echo "[3/8] Building synthetic SYS KEXT test package..."
  DB_SYS_NAME="example-kext-test.sys"
  KEXT_INFO_NAME=""
  KEXT_INFO_COPY=""
  DB_SYS="$RUN/$DB_SYS_NAME"
  bash "$ROOT/tools/sys/build_test_sys_fixture.sh" "$DB_SYS"
fi

echo "[4/8] Compiling APFS/RXFS fixture exporter..."
EXPORTER="$RUN/phase12_fixture_export"
clang++ -D__STDC_HOSTED__=0 -Wno-builtin-macro-redefined \
  -std=c++2c -fno-exceptions -fno-rtti -O0 -g0 -Wall -Wextra \
  -I"$ROOT" -I"$ROOT/include" \
  "$ROOT/tests/stbootmngr_phase12_fixture_export.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/base_system_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/stboot_handoff.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_file_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_path_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_metadata_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/udif_zlib_inflate.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/apfs_reader.cpp" \
  "$ROOT/boot/x64/stbootmngr.efi/src/rxfs_reader.cpp" \
  -o "$EXPORTER"

echo "[5/8] Exporting RXFS -> UDIF -> APFS fixture with KEXT SYS package..."
RXFS_FIXTURE="$RUN/phase12-synthetic-rxfs-16m.img"
if [[ -n "$STLCS_COMPONENT" ]]; then
  cp -- "$STLCS_COMPONENT" "$RUN/stlcs.ect"
fi
if [[ -n "$PHASE13E_KEXT_BUNDLE" ]]; then
  STLCS_ARG="-"
  [[ -z "$STLCS_COMPONENT" ]] || STLCS_ARG="$RUN/stlcs.ect"
  "$EXPORTER" "$BOOT_EFI" "$ECT" "$DB_SYS" "$RXFS_FIXTURE" \
    "$STLCS_ARG" "$KEXT_BUNDLE_NAME" "$KEXT_EXECUTABLE" "$KEXT_INFO_COPY" \
    | tee "$RUN/fixture-export.log"
  grep -Fq "KEXT bundle: intlgpudvs.kext" "$RUN/fixture-export.log"
  grep -Fq "KEXT bundle discovered; Info.plist and multi-block SYS payload read back byte-exact" "$RUN/fixture-export.log"
else
  if [[ -n "$STLCS_COMPONENT" ]]; then
    "$EXPORTER" "$BOOT_EFI" "$ECT" "$DB_SYS" "$RXFS_FIXTURE" "$RUN/stlcs.ect" | tee "$RUN/fixture-export.log"
  else
    "$EXPORTER" "$BOOT_EFI" "$ECT" "$DB_SYS" "$RXFS_FIXTURE" | tee "$RUN/fixture-export.log"
  fi
fi
if [[ -n "$STLCS_COMPONENT" ]]; then
  grep -Fq "PASS: RXFS /sys/User/stlcs.ect path lookup and byte-exact readback" "$RUN/fixture-export.log"
fi
[[ "$(stat -c %s "$RXFS_FIXTURE")" -eq 16777216 ]]

DISK="$RUN/starlix-rxfs-phase12.img"
RXFS_PART="$RUN/starlix-rxfs-phase12.rxfs.img"
ESP="$RUN/starlix-rxfs-phase12.esp.img"
VMDK="$RUN/starlix-rxfs-phase12.vmdk"
VMX="$RUN/starlix-rxfs-phase12.vmx"

echo "[6/8] Copying Phase 10 image into disposable WSL workspace..."
dd if="$PHASE10_DISK" of="$DISK" bs=4M status=progress conv=fsync
DISK_BYTES="$(stat -c %s "$DISK")"
(( DISK_BYTES % 512 == 0 )) || { echo "ABORT: unaligned source image" >&2; exit 1; }
RXFS_START="$(sgdisk -i 2 "$DISK" | awk '/First sector:/ {print $3}')"
RXFS_END="$(sgdisk -i 2 "$DISK" | awk '/Last sector:/ {print $3}')"
RXFS_SECTORS=$((RXFS_END - RXFS_START + 1))
RXFS_BYTES=$((RXFS_SECTORS * 512))
truncate -s "$RXFS_BYTES" "$RXFS_PART"
if (( $(stat -c %s "$RXFS_FIXTURE") > RXFS_BYTES )); then
  echo "ABORT: synthetic RXFS fixture does not fit the RXFS partition" >&2
  exit 1
fi

echo "[7/8] Writing and verifying RXFS partition..."
dd if="$RXFS_FIXTURE" of="$RXFS_PART" bs=4M conv=notrunc status=progress
dd if="$RXFS_PART" of="$DISK" bs=4M seek="$((RXFS_START * 512))" oflag=seek_bytes conv=notrunc status=progress
RXFS_READBACK="$RUN/rxfs-partition.readback.img"
dd if="$DISK" of="$RXFS_READBACK" bs=4M skip="$((RXFS_START * 512))" count="$RXFS_BYTES" iflag=skip_bytes,count_bytes status=progress
cmp "$RXFS_PART" "$RXFS_READBACK"
echo "PASS: RXFS partition payload read-back matches"

ESP_START="$(sgdisk -i 1 "$DISK" | awk '/First sector:/ {print $3}')"
ESP_END="$(sgdisk -i 1 "$DISK" | awk '/Last sector:/ {print $3}')"
ESP_SECTORS=$((ESP_END - ESP_START + 1))
ESP_BYTES=$((ESP_SECTORS * 512))
dd if="$DISK" of="$ESP" bs=4M skip="$((ESP_START * 512))" count="$ESP_BYTES" iflag=skip_bytes,count_bytes status=progress
mcopy -o -i "$ESP" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mmd -i "$ESP" ::/EFI/OpenRPX/EFI
mcopy -o -i "$ESP" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi" ::/EFI/OpenRPX/EFI/cdapfs.efi
dd if="$ESP" of="$DISK" bs=4M seek="$((ESP_START * 512))" oflag=seek_bytes conv=notrunc status=progress

TOTAL_SECTORS=$((DISK_BYTES / 512))
cat > "$VMDK" <<EOF_VMDK
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"
RW $TOTAL_SECTORS FLAT "starlix-rxfs-phase12.img" 0
#DDB
ddb.adapterType = "sata"
ddb.geometry.cylinders = "536"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
EOF_VMDK
cat > "$VMX" <<'EOF_VMX'
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "Starlix OS 1.0 Beta - Phase 12 Controlled Kernel Entry Test"
guestOS = "other-64"
firmware = "efi"
memsize = "1024"
numvcpus = "2"
svga.vramSize = "134217728"
ide0:0.present = "TRUE"
ide0:0.fileName = "starlix-rxfs-phase12.vmdk"
ide0:0.deviceType = "disk"
ide0:0.redo = ""
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "starlix-rxfs-phase12.serial.log"
keyboard.present = "TRUE"
mouse.present = "TRUE"
EOF_VMX

if [[ -n "$PHASE13E_KEXT_BUNDLE" ]]; then
  sed -i 's/Phase 12 Controlled Kernel Entry Test/Phase 13E Intel KEXT Dry-Run/' "$VMX"
elif [[ -n "$PRODUCTION_ECT" ]]; then
  sed -i 's/Controlled Kernel Entry Test/Production Kernel Test/' "$VMX"
fi

echo "[8/8] Validating GPT, ESP files, and SHA-256..."
sgdisk -v "$DISK"
mcopy -i "$ESP" ::/EFI/BOOT/BOOTX64.EFI "$RUN/BOOTX64.EFI.readback"
mcopy -i "$ESP" ::/EFI/OpenRPX/EFI/cdapfs.efi "$RUN/stbtmngr.efi.readback"
cmp "$RUN/BOOTX64.EFI.readback" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI"
cmp "$RUN/stbtmngr.efi.readback" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi"
(
  cd "$RUN"
  HASH_INPUTS=("$ECT_NAME" "$DB_SYS_NAME" phase12-synthetic-rxfs-16m.img \
    starlix-rxfs-phase12.img starlix-rxfs-phase12.vmdk starlix-rxfs-phase12.vmx)
  [[ -z "$KEXT_INFO_NAME" ]] || HASH_INPUTS+=("$KEXT_INFO_NAME")
  [[ ! -f stlcs.ect ]] || HASH_INPUTS+=(stlcs.ect)
  sha256sum "${HASH_INPUTS[@]}" > SHA256SUMS.txt
  sha256sum -c SHA256SUMS.txt
)

echo "[Final] Copying completed candidate into the Windows-accessible project folder..."
dd if="$DISK" of="$OUT_RUN/.starlix-rxfs-phase12.img.partial" bs=4M status=progress conv=fsync
mv "$OUT_RUN/.starlix-rxfs-phase12.img.partial" "$OUT_RUN/starlix-rxfs-phase12.img"
cp "$VMDK" "$OUT_RUN/starlix-rxfs-phase12.vmdk"
cp "$VMX" "$OUT_RUN/starlix-rxfs-phase12.vmx"
cp "$ECT" "$OUT_RUN/$ECT_NAME"
cp "$DB_SYS" "$OUT_RUN/$DB_SYS_NAME"
if [[ -n "$KEXT_INFO_NAME" ]]; then cp "$KEXT_INFO_COPY" "$OUT_RUN/$KEXT_INFO_NAME"; fi
cp "$RXFS_FIXTURE" "$OUT_RUN/phase12-synthetic-rxfs-16m.img"
cp "$RUN/SHA256SUMS.txt" "$OUT_RUN/SHA256SUMS.txt"
cp "$RUN/fixture-export.log" "$OUT_RUN/fixture-export.log"
if [[ -f "$RUN/stlcs.ect" ]]; then cp "$RUN/stlcs.ect" "$OUT_RUN/stlcs.ect"; fi
(
  cd "$OUT_RUN"
  sha256sum -c SHA256SUMS.txt
)

if [[ -n "$PHASE13E_KEXT_BUNDLE" ]]; then
  echo
  echo "PHASE 13E INTEL KEXT PRODUCTION CANDIDATE CREATED"
  echo "KEXT bundle: /System/Library/Extensions/$KEXT_BUNDLE_NAME"
  echo "SYS payload: $DB_SYS_NAME ($(stat -c %s "$DB_SYS") bytes)"
  echo "Info.plist: $KEXT_INFO_NAME"
  echo "Driver entry execution: DISABLED"
  echo "VMX: $OUT_RUN/starlix-rxfs-phase12.vmx"
  echo "Fixture export log: $OUT_RUN/fixture-export.log"
  echo "Inspect VMware serial for RXLOAD KEXT staging and [KEXT-DRYRUN] relocation PASS."
elif [[ -n "$PRODUCTION_ECT" ]]; then
  echo
  if [[ -n "$STLCS_COMPONENT" ]]; then
    echo "PHASE 2N.1 RXFS + STLCS PRODUCTION-KERNEL CANDIDATE CREATED"
    echo "Embedded RXFS component: /sys/User/stlcs.ect"
  else
    echo "PHASE 12 PRODUCTION-KERNEL CANDIDATE CREATED (NO STLCS COMPONENT)"
  fi
  echo "VMX: $OUT_RUN/starlix-rxfs-phase12.vmx"
  echo "Staged kernel ECT: $OUT_RUN/$ECT_NAME"
  echo "Fixture export log: $OUT_RUN/fixture-export.log"
  echo "Inspect serial for ATA IDENTIFY, RXFS mount, and Lock Screen ECT validation."
else
  echo
  echo "PHASE 12 CONTROLLED KERNEL-ENTRY CANDIDATE CREATED"
  echo "VMX: $OUT_RUN/starlix-rxfs-phase12.vmx"
  echo "Expected serial marker: [PHASE12 TEST KERNEL] ENTRY AFTER ExitBootServices"
  echo "This tests loader handoff with a controlled test payload, not the production kernel."
fi
echo "Phase 10 candidate and physical disks were not modified."
echo "Temporary WSL workspace retained: $WORK_ROOT"
