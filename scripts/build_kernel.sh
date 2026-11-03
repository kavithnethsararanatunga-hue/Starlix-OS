#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Kavith Ranatunga

set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KDIR="$ROOT/kernel"
mkdir -p "$KDIR/build/cpu" "$KDIR/build/ob" "$KDIR/build/mem" "$KDIR/build/process" "$KDIR/build/ipc" "$KDIR/build/loader" "$KDIR/build/gui" "$KDIR/build/drivers"

# MAX-DEBUG / NO-SIZE-OPTIMIZATION PROFILE.
# Keep the Ring-0 GPR-only rule. Do not add -Os/-Oz/-O1/-O2/-O3, LTO,
# function folding, section GC, stripping, or synthetic file padding here.
# DWARF stays in kernel.elf; the bootable kernel.bin contains loadable
# sections plus the real BSS extent only.
CXXFLAGS="-target x86_64-unknown-none -I$ROOT/include -ffreestanding -fno-stack-protector -mgeneral-regs-only \
    -fno-exceptions -fno-rtti -mno-red-zone -mcmodel=large \
    -std=c++26 -O0 -g3 -gdwarf-5 -fstandalone-debug \
    -fno-inline -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-lto \
    -fno-merge-all-constants -fno-vectorize -fno-slp-vectorize \
    -fno-function-sections -fno-data-sections -Wall -Wextra"

RING3_SMOKE_TEST="${STARLIX_ENABLE_RING3_SMOKE_TEST:-0}"
case "$RING3_SMOKE_TEST" in
    0) ;;
    1) CXXFLAGS="$CXXFLAGS -DSTARLIX_ENABLE_RING3_SMOKE_TEST=1" ;;
    *) echo "[ERROR] STARLIX_ENABLE_RING3_SMOKE_TEST must be 0 or 1." >&2; exit 2 ;;
esac
if [ "$RING3_SMOKE_TEST" = "1" ]; then
    echo "[!] Building OPT-IN Ring-3 smoke-test kernel (test-only INT 0x80 gate)."
else
    echo "[*] Building normal kernel; Ring-3 smoke test remains disabled."
fi

echo "[*] Compiling Kernel C++ & Assembly sources..."
clang++ $CXXFLAGS -c "$KDIR/src/runtime.cpp" -o "$KDIR/build/runtime.o"
clang++ $CXXFLAGS -c "$KDIR/src/entry.S" -o "$KDIR/build/entry.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/phys.cpp" -o "$KDIR/build/mem/phys.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/managed_page_pool.cpp" -o "$KDIR/build/mem/managed_page_pool.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/address_space_core.cpp" -o "$KDIR/build/mem/address_space_core.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/address_space.cpp" -o "$KDIR/build/mem/address_space.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/kernel_stack.cpp" -o "$KDIR/build/mem/kernel_stack.o"
clang++ $CXXFLAGS -c "$KDIR/src/cpu/gdt_asm.S" -o "$KDIR/build/cpu/gdt_asm.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/cpu/gdt.cpp" -o "$KDIR/build/cpu/gdt.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/cpu/fpu.cpp" -o "$KDIR/build/cpu/fpu.o"
clang++ $CXXFLAGS -c "$KDIR/src/cpu/idt_asm.S" -o "$KDIR/build/cpu/idt_asm.o"
clang++ $CXXFLAGS -c "$KDIR/src/cpu/idt.cpp" -o "$KDIR/build/cpu/idt.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/cpu/exception.cpp" -o "$KDIR/build/cpu/exception.o"
clang++ $CXXFLAGS -c "$KDIR/src/cpu/syscall_asm.S" -o "$KDIR/build/cpu/syscall_asm.o"
clang++ $CXXFLAGS -c "$KDIR/src/cpu/syscall.cpp" -o "$KDIR/build/cpu/syscall.o"
clang++ $CXXFLAGS -c "$KDIR/src/ob/object.cpp" -o "$KDIR/build/ob/object.o"
clang++ $CXXFLAGS -c "$KDIR/src/mem/vmm.cpp" -o "$KDIR/build/mem/vmm.o"
clang++ $CXXFLAGS -c "$KDIR/src/process/process.cpp" -o "$KDIR/build/process/process.o"
clang++ $CXXFLAGS -c "$KDIR/src/process/user_process_transaction.cpp" -o "$KDIR/build/process/user_process_transaction.o"
clang++ $CXXFLAGS -c "$KDIR/src/process/user_process.cpp" -o "$KDIR/build/process/user_process.o"
clang++ $CXXFLAGS -c "$KDIR/src/process/scheduler.cpp" -o "$KDIR/build/process/scheduler.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/process/context_switch.cpp" -o "$KDIR/build/process/context_switch.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/process/user_mode_smoke_test.cpp" -o "$KDIR/build/process/user_mode_smoke_test.o"
clang++ $CXXFLAGS -c "$KDIR/src/ipc/alpc.cpp" -o "$KDIR/build/ipc/alpc.o"
clang++ $CXXFLAGS -c "$KDIR/src/loader/ect_format.cpp" -o "$KDIR/build/loader/ect_format.o"
clang++ $CXXFLAGS -c "$KDIR/src/loader/ect_memory_map.cpp" -o "$KDIR/build/loader/ect_memory_map.o"
clang++ $CXXFLAGS -c "$KDIR/src/loader/ect_loader.cpp" -o "$KDIR/build/loader/ect_loader.o"
clang++ $CXXFLAGS -c "$KDIR/src/loader/user_host_service.cpp" -o "$KDIR/build/loader/user_host_service.o"
clang++ $CXXFLAGS -c "$KDIR/src/loader/system_component.cpp" -o "$KDIR/build/loader/system_component.o"
clang++ $CXXFLAGS -c "$KDIR/src/gui/window.cpp" -o "$KDIR/build/gui/window.o"
clang++ $CXXFLAGS -c "$KDIR/src/drivers/pci.cpp" -o "$KDIR/build/drivers/pci.o"
clang++ $CXXFLAGS -c "$KDIR/src/drivers/pci_resource.cpp" -o "$KDIR/build/drivers/pci_resource.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/drivers/ata_pio.cpp" -o "$KDIR/build/drivers/ata_pio.o"
clang++ $CXXFLAGS -mgeneral-regs-only -c "$KDIR/src/drivers/framebuffer.cpp" -o "$KDIR/build/drivers/framebuffer.o"
clang++ $CXXFLAGS -c "$KDIR/src/drivers/ehci.cpp" -o "$KDIR/build/drivers/ehci.o"
clang++ $CXXFLAGS -c "$KDIR/src/drivers/uhci.cpp" -o "$KDIR/build/drivers/uhci.o"
clang++ $CXXFLAGS -c "$KDIR/src/main.cpp" -o "$KDIR/build/main.o"

echo "[*] Linking Kernel ELF..."
ld.lld -nostdlib -static --no-gc-sections --icf=none --build-id=sha1 \
    -Map="$KDIR/build/kernel.map" -T "$KDIR/linker/kernel.ld" \
    -o "$KDIR/build/kernel.elf" \
    "$KDIR/build/runtime.o" \
    "$KDIR/build/entry.o" \
    "$KDIR/build/mem/phys.o" \
    "$KDIR/build/mem/managed_page_pool.o" \
    "$KDIR/build/mem/address_space_core.o" \
    "$KDIR/build/mem/address_space.o" \
    "$KDIR/build/mem/kernel_stack.o" \
    "$KDIR/build/main.o" \
    "$KDIR/build/cpu/gdt.o" \
    "$KDIR/build/cpu/fpu.o" \
    "$KDIR/build/cpu/gdt_asm.o" \
    "$KDIR/build/cpu/idt.o" \
    "$KDIR/build/cpu/exception.o" \
    "$KDIR/build/cpu/idt_asm.o" \
    "$KDIR/build/cpu/syscall.o" \
    "$KDIR/build/cpu/syscall_asm.o" \
    "$KDIR/build/ob/object.o" \
    "$KDIR/build/mem/vmm.o" \
    "$KDIR/build/process/process.o" \
    "$KDIR/build/process/user_process_transaction.o" \
    "$KDIR/build/process/user_process.o" \
    "$KDIR/build/process/scheduler.o" \
    "$KDIR/build/process/context_switch.o" \
    "$KDIR/build/process/user_mode_smoke_test.o" \
    "$KDIR/build/ipc/alpc.o" \
    "$KDIR/build/loader/ect_format.o" \
    "$KDIR/build/loader/ect_memory_map.o" \
    "$KDIR/build/loader/ect_loader.o" \
    "$KDIR/build/loader/system_component.o" \
    "$KDIR/build/loader/user_host_service.o" \
    "$KDIR/build/gui/window.o" \
    "$KDIR/build/drivers/pci.o" \
    "$KDIR/build/drivers/pci_resource.o" \
    "$KDIR/build/drivers/ata_pio.o" \
    "$KDIR/build/drivers/framebuffer.o" \
    "$KDIR/build/drivers/ehci.o" \
    "$KDIR/build/drivers/uhci.o"

llvm-objcopy -O binary "$KDIR/build/kernel.elf" "$KDIR/build/kernel.bin" 2>/dev/null \
    || objcopy -O binary "$KDIR/build/kernel.elf" "$KDIR/build/kernel.bin"

# Retain developer diagnostics alongside the full, unstripped DWARF ELF.
# These sidecars are not staged into RXFS and are never loaded by the kernel.
if ! readelf -SW "$KDIR/build/kernel.elf" | grep -q '\.debug_info'; then
    echo "[ERROR] Full debug info missing from kernel.elf; refusing a stripped/minimal ELF." >&2
    exit 1
fi
nm -n --demangle "$KDIR/build/kernel.elf" > "$KDIR/build/kernel.symbols.txt"
readelf -SW "$KDIR/build/kernel.elf" > "$KDIR/build/kernel.sections.txt"

# Padding kernel.bin through __starlix_kernel_end reserves zero-initialized BSS in ECT.
KERNEL_END_HEX="$(nm -n "$KDIR/build/kernel.elf" | awk '$3 == "__starlix_kernel_end" { print $1; exit }')"
if [ -z "$KERNEL_END_HEX" ]; then
    echo "[ERROR] Linker symbol __starlix_kernel_end is missing." >&2
    exit 1
fi
KERNEL_END=$((16#$KERNEL_END_HEX))
KERNEL_BASE=$((0x200000))
KERNEL_TARGET_SIZE=$((KERNEL_END - KERNEL_BASE))
if [ "$KERNEL_TARGET_SIZE" -le 0 ]; then
    echo "[ERROR] Invalid linked kernel bounds: end=$KERNEL_END_HEX base=0x200000." >&2
    exit 1
fi
KERNEL_CURRENT_SIZE="$(stat -c %s "$KDIR/build/kernel.bin")"
if [ "$KERNEL_CURRENT_SIZE" -gt "$KERNEL_TARGET_SIZE" ]; then
    echo "[ERROR] kernel.bin is larger than linked image bounds; refusing to truncate." >&2
    exit 1
fi
truncate -s "$KERNEL_TARGET_SIZE" "$KDIR/build/kernel.bin"
echo "[+] kernel.bin padded to $KERNEL_TARGET_SIZE bytes (includes BSS extent)."

echo "[+] Kernel ELF (unstripped DWARF): $KDIR/build/kernel.elf ($(stat -c %s "$KDIR/build/kernel.elf") bytes)"
echo "[+] Kernel binary (loadable sections + BSS): $KDIR/build/kernel.bin ($(stat -c %s "$KDIR/build/kernel.bin") bytes)"
echo "[+] Link map: $KDIR/build/kernel.map"
echo "[+] Symbol map: $KDIR/build/kernel.symbols.txt"
readelf -h "$KDIR/build/kernel.elf" | grep -E "Entry|Class|Machine" || true
