// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "driver/gpu_kext_abi.hpp"

#include <cstdio>

extern "C" StGpuU64 StarlixGpuKextEntry(
    const StarlixGpuKextHostApiV1*, StarlixGpuKextDriverApiV1*);

static bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    else std::printf("PASS: %s\n", message);
    return condition;
}

struct MockResourceHost {
    bool allow_claim{true};
    StPciU64 next_token{0xC001u};
    StPciU32 claims{};
    StPciU32 releases{};
};

static bool mock_claim_bar(void* opaque,
    const StarlixPciDeviceInfoV1* device, StPciU32 bar_index,
    StarlixPciResourceV1* output) {
    auto* mock = static_cast<MockResourceHost*>(opaque);
    if (!mock || !device || !output || !mock->allow_claim || bar_index != 0u)
        return false;
    ++mock->claims;
    *output = StarlixPciResourceV1{};
    output->struct_size = sizeof(*output);
    output->abi_major = STARLIX_PCI_RESOURCE_ABI_MAJOR;
    output->abi_minor = STARLIX_PCI_RESOURCE_ABI_MINOR;
    output->flags = StarlixPciResourceAssignedV1 |
        StarlixPciResourceMemoryV1 | StarlixPciResourceSizeValidatedV1 |
        StarlixPciResourceClaimedV1;
    if ((device->bars[0].flags & StarlixPciBar64BitV1) != 0u)
        output->flags |= StarlixPciResource64BitV1;
    if ((device->bars[0].flags & StarlixPciBarPrefetchableV1) != 0u)
        output->flags |= StarlixPciResourcePrefetchableV1;
    output->bus = device->bus;
    output->device = device->device;
    output->function = device->function;
    output->bar_index = static_cast<StPciU8>(bar_index);
    output->base = device->bars[0].base;
    output->size = 0x00100000ULL;
    output->ownership_token = mock->next_token++;
    return true;
}

static bool mock_release_resource(void* opaque, StPciU64 token) {
    auto* mock = static_cast<MockResourceHost*>(opaque);
    if (!mock || token == 0u) return false;
    ++mock->releases;
    return true;
}

static StarlixPciDeviceInfoV1 make_intel_device() {
    StarlixPciDeviceInfoV1 device{};
    device.struct_size = sizeof(device);
    device.abi_major = STARLIX_PCI_DEVICE_ABI_MAJOR;
    device.abi_minor = STARLIX_PCI_DEVICE_ABI_MINOR;
    device.bus = 0u;
    device.device = 2u;
    device.function = 0u;
    device.header_type = 0u;
    device.vendor_id = 0x8086u;
    device.device_id = 0x0152u;
    device.class_code = 0x03u;
    device.subclass = 0x00u;
    device.flags = StarlixPciDevicePresentV1;
    device.bars[0].base = 0xF0000000ULL;
    device.bars[0].raw_low = 0xF0000008u;
    device.bars[0].flags = StarlixPciBarAssignedV1 |
        StarlixPciBarMemorySpaceV1 | StarlixPciBarPrefetchableV1;
    return device;
}

int main() {
    alignas(16) StGpuU32 framebuffer[64u * 64u]{};
    StarlixGpuHostApi graphics{};
    graphics.struct_size = sizeof(graphics);
    graphics.abi_major = 1u;
    graphics.abi_minor = 0u;
    graphics.pixel_format = StGpuPixelFormatBgrReserved8;
    graphics.framebuffer_base = reinterpret_cast<StGpuU64>(framebuffer);
    graphics.framebuffer_size = sizeof(framebuffer);
    graphics.width = 64u;
    graphics.height = 64u;
    graphics.pixels_per_scanline = 64u;

    StarlixPciDeviceInfoV1 device = make_intel_device();
    MockResourceHost resource_context{};
    StarlixPciResourceHostApiV1 resource_host{};
    resource_host.struct_size = sizeof(resource_host);
    resource_host.abi_major = STARLIX_PCI_RESOURCE_ABI_MAJOR;
    resource_host.abi_minor = STARLIX_PCI_RESOURCE_ABI_MINOR;
    resource_host.context = &resource_context;
    resource_host.claim_bar = &mock_claim_bar;
    resource_host.release_resource = &mock_release_resource;
    StarlixGpuKextHostApiV1 host{};
    host.struct_size = sizeof(host);
    host.abi_major = STARLIX_GPU_KEXT_ABI_MAJOR;
    host.abi_minor = STARLIX_GPU_KEXT_ABI_MINOR;
    host.pci_device = &device;
    host.graphics_host = &graphics;
    host.pci_resources = &resource_host;

    StarlixGpuKextDriverApiV1 api{};
    api.struct_size = sizeof(api);
    bool ok = true;

    ok &= check(StarlixGpuKextEntry(&host, &api) == StarlixGpuKextSuccessV1,
        "external KEXT entry accepts matching Intel HD 2500 descriptor");
    ok &= check(api.pci_vendor_id == 0x8086u && api.pci_device_id == 0x0152u &&
        api.pci_bus == 0u && api.pci_device == 2u && api.pci_function == 0u,
        "external KEXT records matched PCI identity and BDF");
    ok &= check(StarlixPciResourceIsValidV1(&api.bar0_resource) &&
        api.bar0_resource.base == 0xF0000000ULL &&
        api.bar0_resource.size == 0x00100000ULL &&
        api.bar0_resource.ownership_token != 0u && resource_context.claims == 1u,
        "external KEXT claims and records sized BAR0 ownership");
    ok &= check((api.flags & (StarlixGpuKextPciMatchedV1 |
        StarlixGpuKextGopFramebufferReadyV1)) ==
        (StarlixGpuKextPciMatchedV1 | StarlixGpuKextGopFramebufferReadyV1),
        "GOP framebuffer API initializes after PCI descriptor match");
    ok &= check((api.graphics.capabilities & StGpuCapabilityFramePresent) != 0u,
        "graphics ABI v2 presentation methods are exposed");

    device.device_id = 0x1234u;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextUnsupportedDeviceV1,
        "external KEXT rejects a non-target Intel device ID");

    device = make_intel_device();
    device.class_code = 0x02u;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextUnsupportedDeviceV1,
        "external KEXT rejects a non-display PCI class");

    device = make_intel_device();
    device.bars[0].flags = StarlixPciBarIoSpaceV1 | StarlixPciBarAssignedV1;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextUnsupportedDeviceV1,
        "external KEXT rejects an I/O-space BAR0");

    device = make_intel_device();
    device.bars[0].flags = StarlixPciBarUnassignedV1;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextUnsupportedDeviceV1,
        "external KEXT rejects an unassigned BAR0");

    device = make_intel_device();
    resource_context.allow_claim = false;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextBar0UnavailableV1,
        "external KEXT rejects the device when generic BAR0 claim fails");
    resource_context.allow_claim = true;

    device = make_intel_device();
    host.abi_major = 99u;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextAbiMismatchV1,
        "external KEXT rejects a host ABI major mismatch");

    host.abi_major = STARLIX_GPU_KEXT_ABI_MAJOR;
    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    const auto* saved_resource_host = host.pci_resources;
    host.pci_resources = nullptr;
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextInvalidArgumentV1,
        "external KEXT rejects a missing generic PCI resource host API");
    host.pci_resources = saved_resource_host;

    api = StarlixGpuKextDriverApiV1{};
    api.struct_size = sizeof(api);
    host.pci_device = nullptr;
    ok &= check(StarlixGpuKextEntry(&host, &api) ==
        StarlixGpuKextInvalidArgumentV1,
        "external KEXT rejects a missing PCI descriptor");

    std::puts("NOTE: host-only ABI test; no PCI I/O, BAR access, MMIO or GPU execution.");
    return ok ? 0 : 1;
}
