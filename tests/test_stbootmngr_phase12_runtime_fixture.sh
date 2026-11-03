#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
python3 -m py_compile \
  tools/apply_stbootmngr_phase12_runtime_fixture.py \
  tools/build_stbootmngr_phase12_test_ect.py
bash -n tools/build_stbootmngr_phase12_runtime_candidate.sh
TMP="$(mktemp -d /tmp/starlix-phase12-ect-selfcheck.XXXXXX)"
python3 "$ROOT/tools/build_stbootmngr_phase12_test_ect.py" "$TMP/test.ect"
python3 - "$TMP/test.ect" <<'PY'
from pathlib import Path
import struct
import sys
b = Path(sys.argv[1]).read_bytes()
assert b[:2] == b"Px"
assert struct.unpack_from("<H", b, 80)[0] == 128
assert struct.unpack_from("<I", b, 82)[0] == 1
assert struct.unpack_from("<I", b, 90)[0] == 0
assert struct.unpack_from("<I", b, 98)[0] == 0
assert struct.unpack_from("<Q", b, 106)[0] == 0x200000
assert struct.unpack_from("<I", b, 140)[0] == len(b) - 152
print("PASS: generated ECT format and fixed entry point")
PY
rm -rf "$TMP"
echo "PASS: Phase 12 runtime fixture helper syntax and ECT self-check"
