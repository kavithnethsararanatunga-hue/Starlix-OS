// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "syscall.hpp"
#include "gdt.hpp"

namespace starlix::cpu {

constexpr size_t MAX_SYSCALLS = 16;
constexpr size_t SYSCALL_STACK_SIZE = 64 * 1024;

constexpr uint64_t STATUS_INVALID_SYSTEM_SERVICE =
    0xFFFFFFFFFFFFFFFFULL;

constexpr uint64_t EFER_SCE = 1ULL;
constexpr uint64_t SFMASK_IF_DF = (1ULL << 9) | (1ULL << 10);

typedef struct alignas(16) SyscallCpuLocal {
    uint64_t kernel_stack_top;
    uint64_t saved_user_rsp;
} SyscallCpuLocal;

static_assert(sizeof(SyscallCpuLocal) == 16);

extern "C" {
alignas(16) SyscallCpuLocal starlix_syscall_cpu_local{};

alignas(4096) uint8_t starlix_syscall_stack[SYSCALL_STACK_SIZE]
    __attribute__((section(".bss.syscall_stack")));
}

static SyscallServiceFn StNtSyscallTable[MAX_SYSCALLS];

extern "C" void asm_syscall_handler();
extern "C" uint64_t StNtSystemCallHandler(
    uint64_t syscall_num,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3
);

static void cpuid(
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t& eax,
    uint32_t& ebx,
    uint32_t& ecx,
    uint32_t& edx
) {
    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
    );
}

static bool cpu_supports_syscall_msrs() {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    cpuid(1, 0, eax, ebx, ecx, edx);

    // CPUID.01H:EDX[5] = MSR instruction support.
    if ((edx & (1U << 5)) == 0)
        return false;

    cpuid(0x80000000U, 0, eax, ebx, ecx, edx);

    if (eax < 0x80000001U)
        return false;

    cpuid(0x80000001U, 0, eax, ebx, ecx, edx);

    // CPUID.80000001H:EDX[11] = SYSCALL/SYSRET support.
    return (edx & (1U << 11)) != 0;
}

// Only the non-pointer Yield service is enabled in Phase 1B.
static uint64_t SysStNtYield(
    uint64_t a1,
    uint64_t a2,
    uint64_t a3
) {
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

void syscall_table_init() {
    for (size_t i = 0; i < MAX_SYSCALLS; ++i)
        StNtSyscallTable[i] = nullptr;

    StNtSyscallTable[1] = SysStNtYield;

    // Pointer-taking services remain deliberately unavailable:
    // 2 DisplayString
    // 3 AllocateVirtualMemory
    // 4 CreateProcess
    // 6 CreatePort
    // 8 LoadExecutable
}

extern "C" uint64_t StNtSystemCallHandler(
    uint64_t syscall_num,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3
) {
    if (syscall_num > 0 &&
        syscall_num < MAX_SYSCALLS &&
        StNtSyscallTable[syscall_num] != nullptr) {
        return StNtSyscallTable[syscall_num](arg1, arg2, arg3);
    }

    return STATUS_INVALID_SYSTEM_SERVICE;
}

bool syscall_init() {
    syscall_table_init();

    if (!cpu_supports_syscall_msrs())
        return false;

    // Disable SCE while configuring entry state.
    const uint64_t original_efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, original_efer & ~EFER_SCE);

    const uint64_t stack_bottom =
        reinterpret_cast<uint64_t>(&starlix_syscall_stack[0]);

    const uint64_t stack_top =
        stack_bottom + sizeof(starlix_syscall_stack);

    if ((stack_bottom & 4095ULL) != 0 ||
        (stack_top & 15ULL) != 0 ||
        stack_top <= stack_bottom ||
        stack_top - stack_bottom != SYSCALL_STACK_SIZE) {
        return false;
    }

    starlix_syscall_cpu_local.kernel_stack_top = stack_top;
    starlix_syscall_cpu_local.saved_user_rsp = 0;

    const uint64_t cpu_local_address =
        reinterpret_cast<uint64_t>(&starlix_syscall_cpu_local);

    // Kernel GS is active in the current boot context.
    // A future Ring 3 transition must arrange the user GS state.
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_GS_BASE, cpu_local_address);

    if (rdmsr(MSR_GS_BASE) != cpu_local_address ||
        rdmsr(MSR_KERNEL_GS_BASE) != 0) {
        return false;
    }

    // STAR:
    // SYSCALL CS = 0x08, SS = 0x10.
    // SYSRET CS = 0x23, SS = 0x1B.
    const uint16_t user_code_base =
        static_cast<uint16_t>(USER_CODE_SEGMENT & 0xFFF8U);
    const uint16_t sysret_selector_base =
        static_cast<uint16_t>(user_code_base - 0x10U);

    const uint64_t star =
        (static_cast<uint64_t>(sysret_selector_base) << 48) |
        (static_cast<uint64_t>(KERNEL_CODE_SEGMENT) << 32);

    const uint64_t handler_address =
        reinterpret_cast<uint64_t>(asm_syscall_handler);

    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, handler_address);

    // Clear IF and DF on entry. Assembly also executes CLD.
    wrmsr(MSR_SFMASK, SFMASK_IF_DF);

    if (rdmsr(MSR_STAR) != star ||
        rdmsr(MSR_LSTAR) != handler_address ||
        rdmsr(MSR_SFMASK) != SFMASK_IF_DF) {
        return false;
    }

    // Verify the dispatcher without executing the SYSCALL instruction.
    constexpr uint64_t invalid = STATUS_INVALID_SYSTEM_SERVICE;

    if (StNtSystemCallHandler(1, 0, 0, 0) != 0)
        return false;

    constexpr uint64_t disabled_services[] = {0, 2, 3, 4, 6, 8, 16};

    for (uint64_t id : disabled_services) {
        if (StNtSystemCallHandler(id, 0, 0, 0) != invalid)
            return false;
    }

    // Phase 1B: Ring 3 remains disabled.
    // Keep SCE clear so Ring 0 cannot enter the user-only SWAPGS path.
    const uint64_t final_efer = rdmsr(MSR_EFER);

    if ((final_efer & EFER_SCE) != 0)
        wrmsr(MSR_EFER, final_efer & ~EFER_SCE);

    if ((rdmsr(MSR_EFER) & EFER_SCE) != 0)
        return false;

    return true;
}

} // namespace starlix::cpu
