// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::loader {

enum class LockScreenStartupResult {
    Validated,
    RxfsUnavailable,
    ComponentNotFound,
    DuplicateComponent,
    InvalidFileSize,
    FileReadFailed,
    InvalidEct,
    UserServiceBridgeUnavailable,
    UserProcessCreationFailed,
    HostBridgeRegistrationFailed,
    SplashFadeUnavailable,
    FirstEntryRejected,
};

/* Diagnostic-only preflight: discovers and validates the production ECT but
   does not map, create, admit, or enter a user process. */
LockScreenStartupResult stlcs_production_preflight();

/* Production path: read/validate RXFS ECT, create the real user process,
   bind the restricted HostApi gate, and enter its actual bootstrap via IRETQ.
   A successful Ring-3 entry does not return to this call site. */
LockScreenStartupResult stlcs_production_launch();

} // namespace starlix::loader
