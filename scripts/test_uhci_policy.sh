#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"${CXX:-clang++}" -std=c++26 -Wall -Wextra -Werror -I "$ROOT/kernel/src" \
    "$ROOT/tests/Starlix_UHCI_Policy_Test.cpp" -o "$TMP/uhci_policy_test"
"$TMP/uhci_policy_test"
"${CXX:-clang++}" -std=c++26 -Wall -Wextra -Werror -I "$ROOT/kernel/src" \
    "$ROOT/tests/Starlix_UHCI_Hub_Policy_Test.cpp" -o "$TMP/uhci_hub_policy_test"
"$TMP/uhci_hub_policy_test"
echo "PASS: UHCI PCI BAR, DMA page boundary, TD status/link, root-port and USB hub routing/status policy"
