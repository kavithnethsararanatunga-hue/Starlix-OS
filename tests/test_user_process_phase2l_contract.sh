#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/kernel/src/process/user_process.cpp"
TX="$ROOT/kernel/src/process/user_process_transaction.cpp"
BUILD="$ROOT/scripts/build_kernel.sh"
fail() { echo "[FAIL] $1" >&2; exit 1; }
need() { grep -Fq -- "$2" "$1" || fail "$3"; echo "[PASS] $3"; }
need "$SRC" 'context_switch::context_switch_armed()' 'process creation refuses an already-armed first-user context switch'
need "$SRC" 'address_space_active_root() !=' 'process creation requires the kernel address space to be active'
need "$SRC" 'StNtMapExecutable(' 'ECT images use the validated executable mapper'
need "$SRC" 'USER_PAGE_READ | USER_PAGE_WRITE' 'user stack is mapped read/write and non-executable'
need "$SRC" 'kernel_stack_bind_thread(' 'kernel stack is bound before admission'
need "$SRC" 'scheduler::scheduler_admit(record->thread)' 'READY thread is admitted to the scheduler'
need "$SRC" 'process_abort_creation(record->process, record->thread)' 'failed creation can remove unpublished registry objects'
need "$TX" 'USER_PROCESS_STAGE_SCHEDULER' 'scheduler admission is tracked as a rollback stage'
need "$TX" 'USER_PROCESS_STAGE_ADDRESS_SPACE' 'address-space ownership is tracked transactionally'
CREATE_BODY="$(sed -n '/bool StNtCreateUserProcessFromECT/,/bool StNtStartUserProcess/p' "$SRC")"
if grep -Fq 'context_switch_start_first_user' <<<"$CREATE_BODY"; then
  fail 'process creation must remain separate from first-user launch'
fi
need "$SRC" 'bool StNtStartUserProcess(const UserProcessHandle* handle)' 'Phase 2M exposes an explicit separate first-user launch API'
echo '[PASS] process creation remains non-launching; first-user entry is explicit'
need "$BUILD" 'user_process.cpp' 'kernel build compiles and links the process-preparation implementation'
echo 'USER_PROCESS_PHASE2L_CONTRACT: PASS'
