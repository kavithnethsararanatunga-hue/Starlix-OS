#!/usr/bin/env bash
# Builds a disposable RXFS -> synthetic UDIF/APFS -> valid test EFI-child VM image.
# Only writes below build/candidate_rxfs_phase11; never writes physical disks or image/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <phase10-run-directory>" >&2
  exit 2
fi
PHASE10_RUN="$(cd "$1" && pwd)"
PHASE10_DISK="$PHASE10_RUN/starlix-rxfs-phase10.img"
for f in "$PHASE10_DISK" "$ROOT/scripts/build.sh" "$ROOT/RXFS/include/rxfs_format.hpp"; do
  [[ -f "$f" ]] || { echo "Required file missing: $f" >&2; exit 1; }
done
for cmd in clang++ lld-link sgdisk mcopy dd file sha256sum cmp awk; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 1; }
done

BASE="$ROOT/build/candidate_rxfs_phase11"
mkdir -p "$BASE"
STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_RUN="$BASE/run_$STAMP"
mkdir "$OUT_RUN"

# Perform disk construction on the native WSL Linux filesystem.
# Only final VMware deliverables will be copied into the Windows project tree.
WORK_ROOT="$(mktemp -d /tmp/starlix-phase11.XXXXXX)"
RUN="$WORK_ROOT/run_$STAMP"
mkdir "$RUN"
echo "[Phase 11] Working directory on WSL Linux filesystem: $RUN"
echo "[Phase 11] Final output directory: $OUT_RUN"
CHILD_OBJ="$RUN/phase11_child.o"
CHILD_EFI="$RUN/phase11_child.efi"
EXPORTER="$RUN/phase11_fixture_export"
RXFS_FIXTURE="$RUN/phase11-synthetic-rxfs-4m.img"
RXFS_PART="$RUN/starlix-rxfs-phase11.rxfs.img"
DISK="$RUN/starlix-rxfs-phase11.img"
ESP="$RUN/starlix-rxfs-phase11.esp.img"
VMDK="$RUN/starlix-rxfs-phase11.vmdk"
VMX="$RUN/starlix-rxfs-phase11.vmx"

# Ensure the instrumented parent and all reader objects reflect current sources.
bash "$ROOT/scripts/build.sh"

# Build a minimal valid PE32+ EFI child. It validates the parent-provided
# filesystem handoff and returns EFI_SUCCESS only when Lookup/GetInfo/Read pass.
clang++ -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
  -mno-red-zone -fno-stack-protector -fno-exceptions -fno-rtti \
  -std=c++26 -O2 -g0 -fno-lto -Wall -Wextra \
  -I"$ROOT/include" -I"$ROOT" -I"$ROOT/boot/x64/stbootmngr.efi/src" \
  -c "$ROOT/tests/stbootmngr_phase11_child.cpp" -o "$CHILD_OBJ"
lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
  /opt:ref /opt:icf "$CHILD_OBJ" -out:"$CHILD_EFI"
file "$CHILD_EFI"
CHILD_BYTES="$(stat -c %s "$CHILD_EFI")"
if (( CHILD_BYTES > 4096 )); then
  echo "ABORT: test child is $CHILD_BYTES bytes; APFS fixture stores at most 4096 bytes." >&2
  exit 1
fi

# Export the same APFS/UDIF/RXFS structures used by the E2E self-test, replacing
# only boot.efi's 4100-byte APFS payload with a real PE test child and zero pad.
clang++ -D__STDC_HOSTED__=0 -Wno-builtin-macro-redefined -std=c++2c \
  -fno-exceptions -fno-rtti -O0 -g0 -Wall -Wextra \
  -I"$ROOT" -I"$ROOT/include" \
  "$ROOT/tests/stbootmngr_phase11_fixture_export.cpp" \
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
"$EXPORTER" "$CHILD_EFI" "$RXFS_FIXTURE"

# Copy the successful Phase 10 disk, then replace only the RXFS test partition
# in the new copy. The Phase 10 disk remains untouched.
echo "[1/6] Copying Phase 10 disk into temporary WSL workspace..."
dd if="$PHASE10_DISK" of="$DISK" bs=4M status=progress conv=fsync
DISK_BYTES="$(stat -c %s "$DISK")"
(( DISK_BYTES % 512 == 0 )) || { echo 'ABORT: unaligned source disk' >&2; exit 1; }
RXFS_START="$(sgdisk -i 2 "$DISK" | awk '/First sector:/ {print $3}')"
RXFS_END="$(sgdisk -i 2 "$DISK" | awk '/Last sector:/ {print $3}')"
RXFS_SECTORS=$((RXFS_END - RXFS_START + 1))
RXFS_BYTES=$((RXFS_SECTORS * 512))
truncate -s "$RXFS_BYTES" "$RXFS_PART"
if (( $(stat -c %s "$RXFS_FIXTURE") > RXFS_BYTES )); then
  echo 'ABORT: fixture does not fit RXFS partition' >&2
  exit 1
fi
echo "[2/6] Writing synthetic RXFS fixture into the temporary partition image..."
dd if="$RXFS_FIXTURE" of="$RXFS_PART" bs=4M conv=notrunc status=progress

echo "[3/6] Writing RXFS partition into the candidate disk..."
dd if="$RXFS_PART" of="$DISK" bs=4M seek="$((RXFS_START * 512))" oflag=seek_bytes conv=notrunc status=progress

RXFS_READBACK="$RUN/rxfs-partition.readback.img"
echo "[4/6] Reading RXFS partition back for verification..."
dd if="$DISK" of="$RXFS_READBACK" bs=4M skip="$((RXFS_START * 512))" count="$RXFS_BYTES" iflag=skip_bytes,count_bytes status=progress
cmp "$RXFS_PART" "$RXFS_READBACK"
echo "PASS: RXFS partition payload read-back matches the synthetic fixture"

# Refresh the ESP in this new candidate with the current instrumented binaries.
ESP_START="$(sgdisk -i 1 "$DISK" | awk '/First sector:/ {print $3}')"
ESP_END="$(sgdisk -i 1 "$DISK" | awk '/Last sector:/ {print $3}')"
ESP_SECTORS=$((ESP_END - ESP_START + 1))
ESP_BYTES=$((ESP_SECTORS * 512))
echo "[5/6] Extracting ESP into the temporary workspace..."
dd if="$DISK" of="$ESP" bs=4M skip="$((ESP_START * 512))" count="$ESP_BYTES" iflag=skip_bytes,count_bytes status=progress
mcopy -o -i "$ESP" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -o -i "$ESP" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi" ::/EFI/OpenRPX/xsOft/Boot/stbtmngr.efi
echo "[6/6] Updating ESP in the temporary candidate disk..."
dd if="$ESP" of="$DISK" bs=4M seek="$((ESP_START * 512))" oflag=seek_bytes conv=notrunc status=progress

TOTAL_SECTORS=$((DISK_BYTES / 512))
cat > "$VMDK" <<EOF
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"
RW $TOTAL_SECTORS FLAT "starlix-rxfs-phase11.img" 0
#DDB
ddb.adapterType = "sata"
ddb.geometry.cylinders = "536"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
EOF
cat > "$VMX" <<'EOF'
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "Starlix OS 1.0 Beta - Phase 11 Synthetic BaseSystem Handoff Test"
guestOS = "other-64"
firmware = "efi"
memsize = "1024"
numvcpus = "2"
svga.vramSize = "134217728"
ide0:0.present = "TRUE"
ide0:0.fileName = "starlix-rxfs-phase11.vmdk"
ide0:0.deviceType = "disk"
ide0:0.redo = ""
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "starlix-rxfs-phase11.serial.log"
keyboard.present = "TRUE"
mouse.present = "TRUE"
EOF

sgdisk -v "$DISK"
mcopy -i "$ESP" ::/EFI/BOOT/BOOTX64.EFI "$RUN/BOOTX64.EFI.readback"
mcopy -i "$ESP" ::/EFI/OpenRPX/xsOft/Boot/stbtmngr.efi "$RUN/stbtmngr.efi.readback"
cmp "$RUN/BOOTX64.EFI.readback" "$ROOT/boot/x64/bootx64.efi/build/BOOTX64.EFI"
cmp "$RUN/stbtmngr.efi.readback" "$ROOT/boot/x64/stbootmngr.efi/build/stbtmngr.efi"
(
  cd "$RUN"
  sha256sum phase11_child.efi phase11-synthetic-rxfs-4m.img starlix-rxfs-phase11.rxfs.img starlix-rxfs-phase11.esp.img starlix-rxfs-phase11.img > SHA256SUMS.txt
  sha256sum -c SHA256SUMS.txt
)
echo "[Final] Preparing checksums for the bootable candidate..."
(
  cd "$RUN"
  sha256sum phase11_child.efi phase11-synthetic-rxfs-4m.img starlix-rxfs-phase11.img starlix-rxfs-phase11.vmdk starlix-rxfs-phase11.vmx > SHA256SUMS_FINAL.txt
)

echo "[Final] Copying the completed disk into the Windows-accessible project folder..."
FINAL_DISK_TMP="$OUT_RUN/.starlix-rxfs-phase11.img.partial"
dd if="$DISK" of="$FINAL_DISK_TMP" bs=4M status=progress conv=fsync
mv "$FINAL_DISK_TMP" "$OUT_RUN/starlix-rxfs-phase11.img"

cp "$VMDK" "$OUT_RUN/starlix-rxfs-phase11.vmdk"
cp "$VMX" "$OUT_RUN/starlix-rxfs-phase11.vmx"
cp "$CHILD_EFI" "$OUT_RUN/phase11_child.efi"
cp "$RXFS_FIXTURE" "$OUT_RUN/phase11-synthetic-rxfs-4m.img"
cp "$RUN/SHA256SUMS_FINAL.txt" "$OUT_RUN/SHA256SUMS.txt"

echo "[Final] Verifying delivered files..."
(
  cd "$OUT_RUN"
  sha256sum -c SHA256SUMS.txt
)

echo
echo "PHASE 11 TEST-CHILD CANDIDATE CREATED"
echo "VMX: $OUT_RUN/starlix-rxfs-phase11.vmx"
echo "RXFS fixture: $OUT_RUN/phase11-synthetic-rxfs-4m.img"
echo "Expected console message: [PHASE11 CHILD] filesystem handoff PASS"
echo "Expected parent serial marker: child returned EFI_SUCCESS (Phase 11 handoff test passed)"
echo "This tests the synthetic reader/handoff chain, not production BaseSystem boot."
echo "Phase 9/10 candidate disks and physical disks were not modified."
echo "Temporary WSL workspace retained for diagnostics: $WORK_ROOT"
