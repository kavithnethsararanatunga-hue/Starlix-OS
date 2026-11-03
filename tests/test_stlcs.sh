#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tests/stlcs"
mkdir -p "$BUILD"
CXX="${CXX:-clang++}"
FLAGS=(-std=c++20 -O0 -g3 -fno-inline -fno-omit-frame-pointer -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer)
"$CXX" "${FLAGS[@]}" "$ROOT/tests/stlcs_state_test.cpp" \
  "$ROOT/user_components/stlcs/stlcs.cpp" -o "$BUILD/stlcs_state_test"
"$BUILD/stlcs_state_test"
python3 "$ROOT/user_components/stlcs/validate_pic.py" \
  "$ROOT/build/user_components/stlcs/stlcs.elf"
python3 "$ROOT/user_components/stlcs/validate_ect.py" \
  "$ROOT/RXFS/sys/User/stlcs.ect"
