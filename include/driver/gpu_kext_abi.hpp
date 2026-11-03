// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "driver/graphics_abi_v2.hpp"
#include "driver/pci_abi.hpp"
#include "driver/pci_resource_abi.hpp"

constexpr StGpuU32 STARLIX_GPU_KEXT_ABI_MAJOR = 1u;
constexpr StGpuU32 STARLIX_GPU_KEXT_ABI_MINOR = 1u;

enum StarlixGpuKextStatusV1 : StGpuU64 {
    StarlixGpuKextSuccessV1 = 0u,
    StarlixGpuKextInvalidArgumentV1 = 1u,
    StarlixGpuKextAbiMismatchV1 = 2u,
    StarlixGpuKextUnsupportedDeviceV1 = 3u,
    StarlixGpuKextBar0UnavailableV1 = 4u,
    StarlixGpuKextGraphicsInitFailedV1 = 5u
};

enum StarlixGpuKextFlagsV1 : StGpuU32 {
    StarlixGpuKextPciMatchedV1 = 1u << 0,
    StarlixGpuKextGopFramebufferReadyV1 = 1u << 1
};

enum StarlixGpuKextCapabilitiesV1 : StGpuU64 {
    StarlixGpuKextCapabilityGopFramebufferV1 = 1ULL << 0
};

struct StarlixGpuKextHostApiV1 {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 flags;

    const StarlixPciDeviceInfoV1* pci_device;
    const StarlixGpuHostApi* graphics_host;
    const StarlixPciResourceHostApiV1* pci_resources;
};

struct StarlixGpuKextDriverApiV1 {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 flags;

    StGpuU64 capabilities;

    StPciU16 pci_vendor_id;
    StPciU16 pci_device_id;
    StPciU8 pci_bus;
    StPciU8 pci_device;
    StPciU8 pci_function;
    StPciU8 reserved;

    StarlixGpuFrameDriverApi graphics;
    StarlixPciResourceV1 bar0_resource;
};

#ifdef __cplusplus
extern "C" {
#endif
StGpuU64 StarlixGpuKextEntry(
    const StarlixGpuKextHostApiV1*, StarlixGpuKextDriverApiV1*);
#ifdef __cplusplus
}
#endif

static_assert(sizeof(StarlixGpuKextHostApiV1) == 40u,
              "GPU KEXT host ABI v1 size mismatch");
static_assert(sizeof(StarlixGpuKextDriverApiV1) == 144u,
              "GPU KEXT driver ABI v1 size mismatch");
