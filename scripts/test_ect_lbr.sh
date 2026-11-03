#!/bin/bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PASS=0
FAIL=0

check() {
    local desc="$1" cmd="$2" pattern="$3"
    actual=$(eval "$cmd" 2>&1) || true
    if echo "$actual" | grep -Eq "$pattern"; then
        echo "[PASS] $desc"
        PASS=$((PASS+1))
    else
        echo "[FAIL] $desc"
        echo "       expected pattern: $pattern"
        echo "$actual" | sed 's/^/       actual: /'
        FAIL=$((FAIL+1))
    fi
}

echo "=================================================="
echo " Starlix ECT & LBR - Automated Test Suite"
echo "=================================================="

echo; echo "--- Build ---"
bash scripts/build_ect_lbr.sh

echo; echo "--- Create artifacts ---"
./tools/ect/ectpack example.ect
./tools/lbr/lbrpack example.lbr --sign demo-key-2026

echo; echo "--- Compile magic database ---"
file -C -m magic/starlix.magic

echo; echo "--- ECT tests ---"
check "file(1) identifies example.ect" \
      "file -m magic/starlix.magic example.ect" \
      "@Px Starlix ECT Native Executable Pack, x86-64"

check "ect-tool inspect reports PASS" \
      "./tools/ect/ect-tool inspect example.ect" \
      "Validation[[:space:]]*: PASS"

check "starlix-runtime resolves the import and executes the ECT entry" \
      "./tools/ect/starlix-runtime example.ect" \
      "Result of Dynamic Call \(15 \+ 27\) = 42"

check "starlix-runtime completes successfully" \
      "./tools/ect/starlix-runtime example.ect" \
      "Execution Finished"

echo; echo "--- LBR tests ---"
check "file(1) identifies example.lbr" \
      "file -m magic/starlix.magic example.lbr" \
      "@Lb Starlix LBR Native Shared Library, x86-64"

check "lbr-tool inspect reports PASS" \
      "./tools/lbr/lbr-tool inspect example.lbr" \
      "Validation[[:space:]]*: PASS"

check "lbr-tool verify (no key) PASS" \
      "./tools/lbr/lbr-tool verify example.lbr" \
      "Overall[[:space:]]*: PASS"

check "lbr-tool verify (correct key) -> VALID signature" \
      "./tools/lbr/lbr-tool verify example.lbr demo-key-2026" \
      "Signature[[:space:]]*: VALID"

check "lbr-tool verify (wrong key) -> INVALID signature" \
      "./tools/lbr/lbr-tool verify example.lbr wrong-key" \
      "Signature[[:space:]]*: INVALID"

echo; echo "--- Negative / corruption tests ---"
cp example.ect example_corrupt.ect
python3 -c 'import struct; p="example_corrupt.ect"; b=bytearray(open(p,"rb").read()); table=struct.unpack_from("<I",b,86)[0]; code=struct.unpack_from("<I",b,table+16)[0]; b[code]^=0xFF; open(p,"wb").write(b)'

check "ect-tool detects corrupted code" \
      "./tools/ect/ect-tool inspect example_corrupt.ect" \
      "FAIL \(checksum mismatch\)"

check "starlix-runtime refuses corrupted executable code" \
      "./tools/ect/starlix-runtime example_corrupt.ect" \
      "Validation : FAIL \(checksum mismatch\)"

echo
echo "=================================================="
echo " RESULTS: $PASS passed, $FAIL failed"
echo "=================================================="
[ $FAIL -eq 0 ]
