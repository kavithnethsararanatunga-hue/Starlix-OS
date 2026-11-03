// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "pci_abi.hpp"

typedef bool (*StarlixPciClaimBarFnV1)(
    void* context, const StarlixPciDeviceInfoV1* device,
    StPciU32 bar_index, struct StarlixPciResourceV1* output);
typedef bool (*StarlixPciReleaseResourceFnV1)(
    void* context, StPciU64 ownership_token);

constexpr StPciU32 STARLIX_PCI_RESOURCE_ABI_MAJOR = 1u;
constexpr StPciU32 STARLIX_PCI_RESOURCE_ABI_MINOR = 0u;

enum StarlixPciResourceFlagsV1 : StPciU32 {
    StarlixPciResourceAssignedV1 = 1u << 0,
    StarlixPciResourceMemoryV1 = 1u << 1,
    StarlixPciResource64BitV1 = 1u << 2,
    StarlixPciResourcePrefetchableV1 = 1u << 3,
    StarlixPciResourceSizeValidatedV1 = 1u << 4,
    StarlixPciResourceClaimedV1 = 1u << 5
};

struct StarlixPciResourceV1 {
    StPciU32 struct_size;
    StPciU32 abi_major;
    StPciU32 abi_minor;
    StPciU32 flags;
    StPciU8 bus;
    StPciU8 device;
    StPciU8 function;
    StPciU8 bar_index;
    StPciU32 reserved;
    StPciU64 base;
    StPciU64 size;
    StPciU64 ownership_token;
};

struct StarlixPciResourceHostApiV1 {
    StPciU32 struct_size;
    StPciU32 abi_major;
    StPciU32 abi_minor;
    StPciU32 flags;
    void* context;
    StarlixPciClaimBarFnV1 claim_bar;
    StarlixPciReleaseResourceFnV1 release_resource;
};

inline bool StarlixPciResourceRangeValidV1(
    const StarlixPciResourceV1* resource) {
    if (!resource || resource->struct_size < sizeof(StarlixPciResourceV1) ||
        resource->abi_major != STARLIX_PCI_RESOURCE_ABI_MAJOR ||
        resource->abi_minor > STARLIX_PCI_RESOURCE_ABI_MINOR ||
        (resource->flags & (StarlixPciResourceAssignedV1 |
                            StarlixPciResourceMemoryV1 |
                            StarlixPciResourceSizeValidatedV1)) !=
                           (StarlixPciResourceAssignedV1 |
                            StarlixPciResourceMemoryV1 |
                            StarlixPciResourceSizeValidatedV1) ||
        resource->base == 0u || resource->size < 4096u ||
        resource->size > (~static_cast<StPciU64>(0) - resource->base))
        return false;
    return (resource->size & (resource->size - 1u)) == 0u &&
           (resource->base & (resource->size - 1u)) == 0u;
}

inline bool StarlixPciResourceIsValidV1(
    const StarlixPciResourceV1* resource) {
    return StarlixPciResourceRangeValidV1(resource) &&
        (resource->flags & StarlixPciResourceClaimedV1) != 0u &&
        resource->ownership_token != 0u;
}

static_assert(sizeof(StarlixPciResourceV1) == 48u,
              "PCI resource ABI v1 size mismatch");
static_assert(sizeof(StarlixPciResourceHostApiV1) == 40u,
              "PCI resource host API v1 size mismatch");
