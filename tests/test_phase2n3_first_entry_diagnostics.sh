#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTX="$ROOT/kernel/src/process/context_switch.cpp"
HOST="$ROOT/kernel/src/loader/user_host_service.cpp"

need() {
  local file="$1" marker="$2"
  grep -Fq "$marker" "$file" || {
    echo "[FAIL] Missing diagnostic marker: $marker" >&2
    exit 1
  }
  echo "[PASS] $marker"
}

need "$CTX" "FIRST_USER_PREFLIGHT=PASS"
need "$CTX" "FIRST_USER_ROLLBACK=BEGIN"
need "$CTX" "FIRST_USER_TSS_RSP0=PASS"
need "$CTX" "FIRST_USER_CR3_SWITCH=PASS"
need "$CTX" "FIRST_USER_FPU_RESTORE=PASS"
need "$CTX" "FIRST_USER_SEGMENT_BASES=PASS"
need "$CTX" "FIRST_USER_SCHEDULER_REMOVE=PASS"
need "$CTX" "IRETQ_DISPATCH=BEGIN"
need "$HOST" "HOSTAPI_GATE=ENTERED"
need "$HOST" "HOSTAPI_OWNER=PASS"
need "$HOST" "DISPLAY_INFO=PASS"
need "$HOST" "FIRST_DISPLAY_BLIT=PASS"
need "$HOST" "COMPONENT_ACTIVE=PASS"

echo "PHASE2N3_FIRST_ENTRY_DIAGNOSTICS: PASS"
