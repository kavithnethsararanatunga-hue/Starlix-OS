// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t = signed char;
using int16_t = signed short;
using int32_t = signed int;
using int64_t = signed long long;
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using uintptr_t = unsigned long long;
using size_t = unsigned long long;
#endif
#include "system_component.hpp"
#include "ect_loader.hpp"
#include "../storage/storage.hpp"
#include "../drivers/serial.hpp"
#include "../drivers/framebuffer.hpp"
#include "user_host_service.hpp"
#include "../process/user_process.hpp"

namespace starlix::loader {
namespace {

constexpr char kLockScreenPath[] = "RXFS/sys/User/stlcs.ect";
constexpr uint32_t kMaximumComponentBytes = 1024u * 1024u;
alignas(4096) uint8_t g_lock_screen_image[kMaximumComponentBytes]{};

void log(const char* message) {
    starlix::drivers::serial::print(message);
    starlix::drivers::serial::print("\r\n");
}

} // namespace

namespace {

LockScreenStartupResult read_and_validate_lock_screen(uint32_t& out_bytes) {
    out_bytes = 0u;
    const size_t volume_count = starlix::storage::rxfs_volume_count();
    if (volume_count == 0u) {
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=RXFS_NOT_MOUNTED");
        return LockScreenStartupResult::RxfsUnavailable;
    }

    starlix::storage::RxfsVolume selected{};
    starlix::kernel::rxfs::NativeRxfsNode component{};
    size_t matches = 0u;
    for (size_t index = 0u; index < volume_count; ++index) {
        starlix::storage::RxfsVolume volume{};
        starlix::kernel::rxfs::NativeRxfsNode node{};
        if (!starlix::storage::rxfs_volume_get(index, &volume) ||
            !volume.mounted) continue;
        if (!starlix::storage::rxfs_lookup_path(volume, kLockScreenPath,
                                                 &node) ||
            node.file_type != starlix::rxfs::RXFS_TYPE_REGULAR) continue;
        ++matches;
        selected = volume;
        component = node;
    }

    if (matches == 0u) {
        log("[RXFS] LOCKSCREEN_FILE=FAIL; reason=EXACT_PATH_NOT_FOUND");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=COMPONENT_NOT_FOUND");
        return LockScreenStartupResult::ComponentNotFound;
    }
    if (matches != 1u) {
        log("[RXFS] LOCKSCREEN_FILE=FAIL; reason=AMBIGUOUS_ACROSS_MOUNTED_VOLUMES");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=DUPLICATE_COMPONENT");
        return LockScreenStartupResult::DuplicateComponent;
    }
    log("[RXFS] LOCKSCREEN_FILE=FOUND");

    if (component.file_size == 0u ||
        component.file_size > kMaximumComponentBytes) {
        log("[ECT] STLCS_READ=FAIL; reason=FILE_SIZE_OUT_OF_POLICY");
        return LockScreenStartupResult::InvalidFileSize;
    }

    uint32_t bytes_read = 0u;
    if (!starlix::storage::rxfs_read_file(
            selected, kLockScreenPath, g_lock_screen_image,
            kMaximumComponentBytes, &bytes_read) ||
        bytes_read != component.file_size) {
        log("[ECT] STLCS_READ=FAIL; reason=SHORT_READ_OR_RXFS_IO_ERROR");
        return LockScreenStartupResult::FileReadFailed;
    }

    ect::LoadPlan plan{};
    if (!StNtInspectExecutable(g_lock_screen_image, bytes_read, &plan)) {
        log("[ECT] STLCS_VALIDATION=FAIL; reason=HEADER_SECTION_CHECKSUM_OR_POLICY");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=INVALID_ECT");
        return LockScreenStartupResult::InvalidEct;
    }
    log("[ECT] STLCS_HEADER=PASS");
    log("[ECT] STLCS_SECTIONS=PASS");
    log("[ECT] STLCS_ENTRY=VALID");
    out_bytes = bytes_read;
    return LockScreenStartupResult::Validated;
}

} // namespace

LockScreenStartupResult stlcs_production_preflight() {
    uint32_t bytes = 0u;
    const LockScreenStartupResult result = read_and_validate_lock_screen(bytes);
    if (result == LockScreenStartupResult::Validated) {
        log("[ECT] STLCS_MAPPING=NOT_REQUESTED; mode=PREFLIGHT_ONLY");
        return LockScreenStartupResult::Validated;
    }
    return result;
}

LockScreenStartupResult stlcs_production_launch() {
    uint32_t bytes = 0u;
    const LockScreenStartupResult validation = read_and_validate_lock_screen(bytes);
    if (validation != LockScreenStartupResult::Validated) return validation;

    if (!host_service::service_gate_ready()) {
        log("[ECT] STLCS_MAPPING=SKIPPED; reason=USER_HOST_SERVICE_GATE_NOT_READY");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=USER_HOST_SERVICE_BRIDGE_UNAVAILABLE");
        return LockScreenStartupResult::UserServiceBridgeUnavailable;
    }

    starlix::process::UserProcessHandle handle{};
    if (!starlix::process::StNtCreateUserProcessFromECT(
            "stlcs", g_lock_screen_image, bytes, &handle)) {
        log("[PROCESS] STLCS_PROCESS=FAIL; reason=ECT_MAPPING_OR_PROCESS_TRANSACTION");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=USER_PROCESS_CREATION_FAILED");
        return LockScreenStartupResult::UserProcessCreationFailed;
    }
    log("[ECT] STLCS_MAPPING=PASS");
    log("[PROCESS] STLCS_PROCESS=CREATED");
    log("[PROCESS] STLCS_ADDRESS_SPACE=READY");

    starlix::process::StThread* thread =
        starlix::process::user_process_main_thread(&handle);
    if (!thread || !host_service::bind_lock_screen_thread(thread)) {
        if (thread) host_service::clear_lock_screen_thread(thread);
        (void)starlix::process::StNtAbortUserProcess(&handle);
        log("[PROCESS] STLCS_HOST_BRIDGE=FAIL; reason=THREAD_BIND_OR_GATE_CHECK");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=HOST_BRIDGE_REGISTRATION_FAILED");
        return LockScreenStartupResult::HostBridgeRegistrationFailed;
    }
    log("[STACK] STLCS_USER_STACK=READY");
    log("[STACK] STLCS_KERNEL_STACK=READY");
    log("[SCHEDULER] STLCS_THREAD=ADMITTED");
    log("[CONTEXT] STLCS_FIRST_ENTRY=READY");

    /* Fade the inherited UEFI logo/spinner while still in CPL0, after the
       component is ready but immediately before the first CPL3 handoff. The
       framebuffer layer refuses to fake a fade when the PIT/IF is unavailable. */
    if (!starlix::drivers::framebuffer::fade_to_black(660u)) {
        host_service::clear_lock_screen_thread(thread);
        const bool aborted = starlix::process::StNtAbortUserProcess(&handle);
        log(aborted
            ? "[TRANSITION] SPLASH_FADE_OUT=FAIL; reason=PIT_OR_FRAMEBUFFER_UNAVAILABLE; PROCESS_ROLLED_BACK"
            : "[TRANSITION] SPLASH_FADE_OUT=FAIL; reason=PIT_OR_FRAMEBUFFER_UNAVAILABLE; ROLLBACK_INCOMPLETE");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=SPLASH_TRANSITION_UNAVAILABLE");
        return LockScreenStartupResult::SplashFadeUnavailable;
    }
    log("[TRANSITION] SPLASH_FADE_OUT=PASS");

    /* Successful IRETQ is non-returning. On a rejected transition, tear down
       the binding and the still-inactive process transaction fail-closed. */
    if (!starlix::process::StNtStartUserProcess(&handle)) {
        host_service::clear_lock_screen_thread(thread);
        const bool aborted = starlix::process::StNtAbortUserProcess(&handle);
        log(aborted
            ? "[CONTEXT] STLCS_FIRST_ENTRY=FAIL; reason=IRETQ_REJECTED_AND_ROLLED_BACK"
            : "[CONTEXT] STLCS_FIRST_ENTRY=FAIL; reason=IRETQ_REJECTED_ROLLBACK_INCOMPLETE");
        log("[LOCKSCREEN] STARTUP=BLOCKED; reason=FIRST_ENTRY_REJECTED");
        return LockScreenStartupResult::FirstEntryRejected;
    }

    /* Should be unreachable: a validated successful first-entry path transfers
       control to CPL3 and never returns to KernelMain. */
    log("[FATAL] STLCS_FIRST_ENTRY_RETURNED_UNEXPECTEDLY; SYSTEM_REMAINS_LOCKED");
    asm volatile("cli" ::: "memory");
    for (;;) asm volatile("hlt" ::: "memory");
}

} // namespace starlix::loader
