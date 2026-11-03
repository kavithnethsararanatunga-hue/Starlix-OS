#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASM="$ROOT/kernel/src/cpu/idt_asm.S"
CPP="$ROOT/kernel/src/cpu/idt.cpp"
CTX="$ROOT/kernel/src/process/context_switch.cpp"
check() {
  if "$@"; then return 0; else return 1; fi
}
check grep -Fq 'movq %rsp, %rsi' "$ASM" && echo '[PASS] IRQ assembly passes the normalized frame pointer to C++'
check grep -Fq 'testq %rax, %rax' "$ASM" && echo '[PASS] IRQ assembly treats zero as the keep-current-frame sentinel'
check grep -Fq 'movq %rax, %rsp' "$ASM" && echo '[PASS] IRQ assembly can restore from a validated alternate frame'
check grep -Fq 'addq $8, %rsp' "$ASM" && grep -Fq 'iretq' "$ASM" && echo '[PASS] IRQ epilogue discards vector and returns through IRETQ'
check grep -Fq 'if (irq == 0u && frame && (frame->cs & 3ULL) == 3ULL)' "$CPP" && grep -Fq 'context_switch_on_timer(frame)' "$CPP" && echo '[PASS] only an eligible CPL3 PIT interrupt can request a context switch'
check grep -Fq '__atomic_store_n(&g_armed, 0u' "$CTX" && echo '[PASS] context switching remains disarmed until the first validated user entry'
printf 'IRQ_RETURN_CONTRACT_PHASE2F: PASS (6 checks)\n'
