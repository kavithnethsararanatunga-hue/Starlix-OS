/*
 * Starlix EHCI Phase 32: versioned release manifest and fail-closed release gate.
 * This records the supported profile; it does not assert physical USB conformance.
 */
#pragma once

namespace starlix::drivers::ehci::release {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;

static constexpr u16 DriverVersionMajor = 1u;
static constexpr u16 DriverVersionMinor = 0u;
static constexpr u16 DriverVersionPatch = 0u;
static constexpr u16 InterfaceApiMajor = 1u;
static constexpr u16 InterfaceApiMinor = 0u;
static constexpr char ReleaseChannel[] = "beta";

/* The only profile covered by existing DMA evidence. */
enum class DmaProfile : u8 {
    X86_64IdentityMappedCoherentDma32 = 1u
};
static constexpr DmaProfile SupportedDmaProfile =
    DmaProfile::X86_64IdentityMappedCoherentDma32;

/* Safe defaults are explicit and fail closed for unsupported paths. */
static constexpr bool AllowUnknownUsbClasses = false;
static constexpr bool AllowUnlistedVendorInterfaces = false;
static constexpr bool AllowDownstreamHubTtScheduling = false;
static constexpr bool AllowNonIdentityDma = false;
static constexpr bool AllowNonCoherentDma = false;
static constexpr bool AllowPrivilegedFirmwareUpdateInterfaces = false;
static constexpr u32 MaxRequestSlots = 32u;
static constexpr u32 MaxTransferBytes = 16384u;
static constexpr u32 MaxConfigurationDescriptorBytes = 4096u;

struct Manifest {
    u16 driver_major;
    u16 driver_minor;
    u16 driver_patch;
    u16 api_major;
    u16 api_minor;
    DmaProfile dma_profile;
    bool unknown_classes_allowed;
    bool unlisted_vendor_allowed;
    bool downstream_tt_allowed;
    bool non_identity_dma_allowed;
    bool non_coherent_dma_allowed;
    u32 max_request_slots;
    u32 max_transfer_bytes;
    u32 max_configuration_descriptor_bytes;
};

constexpr Manifest manifest() {
    return Manifest{
        DriverVersionMajor, DriverVersionMinor, DriverVersionPatch,
        InterfaceApiMajor, InterfaceApiMinor, SupportedDmaProfile,
        AllowUnknownUsbClasses, AllowUnlistedVendorInterfaces,
        AllowDownstreamHubTtScheduling, AllowNonIdentityDma,
        AllowNonCoherentDma, MaxRequestSlots, MaxTransferBytes,
        MaxConfigurationDescriptorBytes
    };
}

/* Release sign-off is evidence-gated, not inferred from a successful boot. */
struct ExitEvidence {
    bool clean_full_build;
    bool reproducible_vm_boot;
    bool sanitizer_ci;
    bool transfer_error_injection;
    bool physical_usb_class_transactions;
    bool measured_hardware_performance;
    bool known_issues_documented;
    bool rollback_tested;
};

constexpr bool stable_release_allowed(const ExitEvidence& e) {
    return e.clean_full_build && e.reproducible_vm_boot && e.sanitizer_ci &&
           e.transfer_error_injection && e.physical_usb_class_transactions &&
           e.measured_hardware_performance && e.known_issues_documented &&
           e.rollback_tested;
}

constexpr bool manifest_is_safe(const Manifest& m) {
    return m.driver_major == 1u && m.api_major == 1u &&
           m.dma_profile == DmaProfile::X86_64IdentityMappedCoherentDma32 &&
           !m.unknown_classes_allowed && !m.unlisted_vendor_allowed &&
           !m.downstream_tt_allowed && !m.non_identity_dma_allowed &&
           !m.non_coherent_dma_allowed && m.max_request_slots == 32u &&
           m.max_transfer_bytes == 16384u &&
           m.max_configuration_descriptor_bytes == 4096u;
}

constexpr bool self_test() {
    const Manifest m = manifest();
    if (!manifest_is_safe(m)) return false;
    if (DriverVersionMinor != 0u || DriverVersionPatch != 0u ||
        InterfaceApiMinor != 0u) return false;

    const ExitEvidence none{};
    if (stable_release_allowed(none)) return false;
    const ExitEvidence complete{
        true, true, true, true, true, true, true, true
    };
    if (!stable_release_allowed(complete)) return false;

    /* Every independent release criterion must be required. */
    for (unsigned int missing = 0u; missing < 8u; ++missing) {
        ExitEvidence e = complete;
        switch (missing) {
            case 0u: e.clean_full_build = false; break;
            case 1u: e.reproducible_vm_boot = false; break;
            case 2u: e.sanitizer_ci = false; break;
            case 3u: e.transfer_error_injection = false; break;
            case 4u: e.physical_usb_class_transactions = false; break;
            case 5u: e.measured_hardware_performance = false; break;
            case 6u: e.known_issues_documented = false; break;
            default: e.rollback_tested = false; break;
        }
        if (stable_release_allowed(e)) return false;
    }
    return true;
}

} // namespace starlix::drivers::ehci::release
