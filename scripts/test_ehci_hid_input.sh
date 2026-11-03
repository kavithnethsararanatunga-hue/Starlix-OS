#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CXX="${CXX:-clang++}"
"$CXX" -std=c++26 -Wall -Wextra -Werror -I "$ROOT/kernel/src" \
    "$ROOT/tests/Starlix_EHCI_HID_Input_Test.cpp" -o "$TMP/ehci_hid_input_test"
"$TMP/ehci_hid_input_test"
