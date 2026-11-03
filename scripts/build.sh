#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE="$ROOT/include"
COMMON_OBJDIR="$ROOT/src/common/build"
mkdir -p "$COMMON_OBJDIR"

CXXFLAGS="-target x86_64-unknown-windows -ffreestanding -fshort-wchar \
  -mno-red-zone -fno-stack-protector -fno-exceptions -fno-rtti \
  -std=c++26 -O0 -g3 -gcodeview -fno-inline -fno-omit-frame-pointer -fno-lto -Wall -Wextra -I$INCLUDE \
  -I$ROOT/boot/x64/stbootmngr.efi/src \
  -I$ROOT/kernel/src"

LDFLAGS="-subsystem:efi_application -entry:efi_main -nodefaultlib"

echo "[*] runtime.o"
clang++ $CXXFLAGS -c "$ROOT/src/common/runtime.cpp" -o "$COMMON_OBJDIR/runtime.o"

BOOTX64_DIR="$ROOT/boot/x64/bootx64.efi"
echo "[*] BOOTX64.EFI"
clang++ $CXXFLAGS -c "$BOOTX64_DIR/src/main.cpp" -o "$BOOTX64_DIR/build/main.o"
lld-link $LDFLAGS /debug /opt:noref /opt:noicf /pdb:"$BOOTX64_DIR/build/BOOTX64.pdb" "$BOOTX64_DIR/build/main.o" "$COMMON_OBJDIR/runtime.o" -out:"$BOOTX64_DIR/build/BOOTX64.EFI"

RXLOAD_DIR="$ROOT/boot/x64/rxload.efi"
echo "[*] Embedding shared graphical Starlix boot assets..."
python3 "$ROOT/tools/embed_boot_splash_assets.py" \
  "$RXLOAD_DIR/src/assets/Stlg.png" \
  "$RXLOAD_DIR/src/assets/StarlixBootSpinner.svg" \
  "$RXLOAD_DIR/src/boot_splash_assets.hpp"

STBMGR_DIR="$ROOT/boot/x64/stbootmngr.efi"
echo "[*] stbtmngr.efi (persistent graphical splash + RXFS/APFS loader)"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/ssp.cpp" -o "$STBMGR_DIR/build/ssp.o"
# Read-only RXFS/APFS image inspection modules. The main boot path is not switched yet.
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/rxfs_reader.cpp" -o "$STBMGR_DIR/build/rxfs_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_reader.cpp" -o "$STBMGR_DIR/build/apfs_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_reader.cpp" -o "$STBMGR_DIR/build/udif_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/udif_zlib_inflate.cpp" -o "$STBMGR_DIR/build/udif_zlib_inflate.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_metadata_reader.cpp" -o "$STBMGR_DIR/build/apfs_metadata_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_path_reader.cpp" -o "$STBMGR_DIR/build/apfs_path_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/apfs_file_reader.cpp" -o "$STBMGR_DIR/build/apfs_file_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/base_system_reader.cpp" -o "$STBMGR_DIR/build/base_system_reader.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/stboot_handoff.cpp" -o "$STBMGR_DIR/build/stboot_handoff.o"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/main.cpp" -o "$STBMGR_DIR/build/main.o"
lld-link $LDFLAGS /debug /opt:noref /opt:noicf /pdb:"$STBMGR_DIR/build/stbtmngr.pdb" "$STBMGR_DIR/build/main.o" "$STBMGR_DIR/build/stboot_handoff.o" "$STBMGR_DIR/build/ssp.o" "$STBMGR_DIR/build/rxfs_reader.o" "$STBMGR_DIR/build/apfs_reader.o" "$STBMGR_DIR/build/udif_reader.o" "$STBMGR_DIR/build/udif_zlib_inflate.o" "$STBMGR_DIR/build/apfs_metadata_reader.o" "$STBMGR_DIR/build/apfs_path_reader.o" "$STBMGR_DIR/build/apfs_file_reader.o" "$STBMGR_DIR/build/base_system_reader.o" "$COMMON_OBJDIR/runtime.o" \
  -out:"$STBMGR_DIR/build/stbtmngr.efi"

RXLOAD_DIR="$ROOT/boot/x64/rxload.efi"
echo "[*] Rxload.efi (graphical splash + SSP + ECT kernel loader)"
clang++ $CXXFLAGS -c "$STBMGR_DIR/src/ssp.cpp" -o "$RXLOAD_DIR/build/ssp.o"
clang++ $CXXFLAGS -c "$RXLOAD_DIR/src/main.cpp" -o "$RXLOAD_DIR/build/main.o"
lld-link $LDFLAGS /debug /opt:noref /opt:noicf /pdb:"$RXLOAD_DIR/build/Rxload.pdb" "$RXLOAD_DIR/build/main.o" "$RXLOAD_DIR/build/ssp.o" "$COMMON_OBJDIR/runtime.o" \
  -out:"$RXLOAD_DIR/build/Rxload.efi"

# APFS BaseSystem boot stage: same loader source, boot.efi artifact name.
# Keep Rxload.efi built temporarily for the legacy SSP fallback.
lld-link $LDFLAGS /debug /opt:noref /opt:noicf /pdb:"$RXLOAD_DIR/build/boot.pdb" "$RXLOAD_DIR/build/main.o" "$RXLOAD_DIR/build/ssp.o" "$COMMON_OBJDIR/runtime.o" \
  -out:"$RXLOAD_DIR/build/boot.efi"

echo "[✓] EFI build complete"
