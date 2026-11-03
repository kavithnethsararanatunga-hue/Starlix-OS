#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 -m py_compile \
  "$ROOT/tools/apply_stbootmngr_phase12_runtime_fixture.py" \
  "$ROOT/tools/build_stbootmngr_phase12_test_ect.py"
bash -n "$ROOT/tools/build_stbootmngr_phase12_runtime_candidate.sh"
bash -n "$ROOT/tools/build_starlix_phase2m_phase12_candidate.sh"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1])
checks={
 "Phase 12 production ECT limit is 4 MiB": "ECT_BYTES > 4 * 1024 * 1024" in (r/"tools/build_stbootmngr_phase12_runtime_candidate.sh").read_text(),
 "APFS fixture grows to 4 MiB": "BLOCKS = 1024" in (r/"tests/stbootmngr_apfs_path_selftest.cpp").read_text(),
 "ECT fixture capacity is 4 MiB": "ectSize > 4ULL * 1024ULL * 1024ULL" in (r/"tests/stbootmngr_phase12_apfs_fixture.hpp").read_text(),
 "RXFS fixture volume grows to 16 MiB": "E2E_RXFS_BYTES = 16ULL * 1024ULL * 1024ULL" in (r/"tests/stbootmngr_basesystem_selftest.cpp").read_text(),
 "UDIF fixture capacity grows to 5 MiB": "E2E_DMG_CAPACITY = 5ULL * 1024ULL * 1024ULL" in (r/"tests/stbootmngr_basesystem_selftest.cpp").read_text(),
 "RXFS exporter verifies double-indirect readback": "RXFS double-indirect read failed" in (r/"tests/stbootmngr_phase12_fixture_export.cpp").read_text(),
 "candidate wrapper opts in to Phase 2M diagnostic": "STARLIX_ENABLE_RING3_SMOKE_TEST=1 bash scripts/build_kernel.sh" in (r/"tools/build_starlix_phase2m_phase12_candidate.sh").read_text(),
 "candidate uses existing Phase 12 pipeline": "build_stbootmngr_phase12_runtime_candidate.sh" in (r/"tools/build_starlix_phase2m_phase12_candidate.sh").read_text(),
}
for k,v in checks.items():
 print(f"[{'PASS' if v else 'FAIL'}] {k}")
if not all(checks.values()): raise SystemExit(1)
PY
bash "$ROOT/tests/test_stbootmngr_phase12_runtime_fixture.sh"
echo "PASS: Phase 2M / Phase 12 capacity contract"
