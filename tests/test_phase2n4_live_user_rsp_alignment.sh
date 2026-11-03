#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run() { echo "===== $1 ====="; bash "$ROOT/$1"; }

# The lock screen's exact PIC build must be current before packaging checks.
run scripts/build_stlcs.sh
run tests/test_interrupt_frame_phase2e.sh
run tests/test_context_switch_phase2f.sh
run tests/test_user_process_start_phase2m_contract.sh
run tests/test_phase2n3_first_entry_diagnostics.sh
run tests/test_stlcs.sh
run tests/test_phase2n2_host_service_contract.sh
run tests/test_phase2n2_host_service_policy.sh
run tests/test_phase2n2_framebuffer_policy.sh
run tests/test_ect_loader_phase2j.sh
run tests/test_phase2n1_rxfs_stlcs_fixture.sh

# Rebuild the normal production kernel (Ring-3 smoke option remains disabled).
run scripts/build_kernel.sh

echo "PHASE2N4_LIVE_USER_RSP_ALIGNMENT: PASS"
