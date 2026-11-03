#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[*] Building Full-Size System Libraries (.lbr)..."
clang++ -std=c++26 -O0 -c -fPIC "$ROOT/user_lib/libstarlix.cpp" -o "$ROOT/user_lib/libstarlix.o"
"$ROOT/tools/lbr/elf2lbr" "$ROOT/user_lib/libstarlix.o" "$ROOT/system_root/Starlix/System/libstarlix.lbr"

echo "[*] Building Full-Size Graphics Driver (strlxgui.sys)..."
clang++ -std=c++26 -O0 -c -fPIC "$ROOT/system_root/Starlix/System/Drivers/strlxgui.cpp" -o "$ROOT/system_root/Starlix/System/Drivers/strlxgui.o"
"$ROOT/tools/lbr/elf2lbr" "$ROOT/system_root/Starlix/System/Drivers/strlxgui.o" "$ROOT/system_root/Starlix/System/Drivers/strlxgui.sys"

echo "[*] Building Full-Size USB EHCI Driver (ehci.sys)..."
clang++ -std=c++26 -O0 -c -fPIC "$ROOT/system_root/Starlix/System/Drivers/ehci_standalone.cpp" -o "$ROOT/system_root/Starlix/System/Drivers/ehci_standalone.o"
"$ROOT/tools/lbr/elf2lbr" "$ROOT/system_root/Starlix/System/Drivers/ehci_standalone.o" "$ROOT/system_root/Starlix/System/Drivers/ehci.sys"

echo "[*] Building Full-Size User Apps (explorer.ect)..."
clang++ -std=c++26 -O0 -c -fPIC "$ROOT/system_root/Starlix/System/sysapps/explorer.cpp" -o "$ROOT/system_root/Starlix/System/sysapps/explorer.o"
"$ROOT/tools/ect/elf2ect" "$ROOT/system_root/Starlix/System/sysapps/explorer.o" "$ROOT/system_root/Starlix/System/sysapps/explorer.ect" --dep "strlxgui.sys"

echo "[✓] Full-Size Drivers & Apps Built Successfully!"
