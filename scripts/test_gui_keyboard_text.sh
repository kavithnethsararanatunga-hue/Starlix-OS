#!/usr/bin/env bash
set -euo pipefail
P="/mnt/c/Users/User/Desktop/OpenRPX/xsOft/SFTsrc/OS/Starlix/1.0.Beta"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -Wall -Wextra -Werror \
    "$P/tests/Starlix_GUI_Keyboard_Text_Test.cpp" \
    -o "$TMP/gui-keyboard-text-test"
"$TMP/gui-keyboard-text-test"
