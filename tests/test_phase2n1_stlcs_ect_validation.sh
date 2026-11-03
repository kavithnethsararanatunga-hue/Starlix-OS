#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for cmd in clang++ g++ sha256sum; do
  command -v "$cmd" >/dev/null || { echo "Missing required tool: $cmd" >&2; exit 2; }
done
STLCS=""
for candidate in "$ROOT/RXFS/sys/User/stlcs.ect" "$ROOT/rxfs/sys/User/stlcs.ect"; do
  if [[ -s "$candidate" ]]; then STLCS="$candidate"; break; fi
done
[[ -n "$STLCS" ]] || { echo "ABORT: production stlcs.ect not found" >&2; exit 1; }
TMP="$(mktemp -d /tmp/starlix-phase2n1-ect.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/validate.cpp" <<'CPP'
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
#include "kernel/src/loader/ect_format.hpp"
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 3;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    starlix::loader::ect::LoadPlan plan{};
    if (!starlix::loader::ect::validate_image(bytes.data(), bytes.size(), plan)) {
        std::fprintf(stderr, "FAIL: production ECT rejected by kernel ECT v1 parser\n");
        return 4;
    }
    if (plan.section_count == 0 || plan.entry_point == 0 || plan.image_base == 0) return 5;
    std::printf("PASS: kernel ECT v1 parser accepted stlcs.ect (%zu bytes; %u sections; entry=0x%llx)\n",
        bytes.size(), plan.section_count, static_cast<unsigned long long>(plan.entry_point));
    return 0;
}
CPP
for compiler in clang++ g++; do
  command -v "$compiler" >/dev/null || continue
  echo "[$compiler] kernel parser host test with ASan/UBSan"
  "$compiler" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$ROOT" "$TMP/validate.cpp" "$ROOT/kernel/src/loader/ect_format.cpp" \
    -o "$TMP/validate-${compiler//+/p}"
  "$TMP/validate-${compiler//+/p}" "$STLCS"
done
sha256sum "$STLCS"
