// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "driver/graphics_abi_v2.hpp"
#include "driver/gpu_kext_abi.hpp"
#include "intel_gpu_pci_policy.hpp"

extern "C" StGpuU64 StarlixIntelGpuMatchesPciId(
    StGpuU32 vendorId, StGpuU32 deviceId) {
    return starlix::drivers::intel_gpu::IsSupportedKextDevice(
        vendorId, deviceId) ? 1ULL : 0ULL;
}

static bool StIntelGpuValidHost(const StarlixGpuHostApi* host) {
    if (!host ||
        host->struct_size < sizeof(StarlixGpuHostApi) ||
        host->abi_major != 1)
        return false;

    if (!host->framebuffer_base ||
        (host->framebuffer_base & 3ULL) != 0 ||
        host->framebuffer_size < 4 ||
        !host->width || !host->height ||
        !host->pixels_per_scanline ||
        host->pixels_per_scanline < host->width)
        return false;

    if (host->pixel_format != StGpuPixelFormatRgbReserved8 &&
        host->pixel_format != StGpuPixelFormatBgrReserved8)
        return false;

    const StGpuU64 capacity = host->framebuffer_size / 4;
    if (host->height > capacity / host->pixels_per_scanline)
        return false;

    // Reject address ranges that would overflow the 64-bit address space.
    const StGpuU64 maxValue = ~static_cast<StGpuU64>(0);
    if (host->framebuffer_base > maxValue - host->framebuffer_size)
        return false;

    return true;
}

static StGpuU32 StIntelGpuConvertColor(
    const StarlixGpuHostApi* host, StGpuU32 color) {
    color &= 0x00FFFFFFu;

    // Input is 0x00RRGGBB. GOP RGB byte order needs red/blue swap
    // on little-endian x86-64; BGR byte order uses the value directly.
    if (host->pixel_format == StGpuPixelFormatRgbReserved8) {
        return ((color & 0x000000FFu) << 16) |
               (color & 0x0000FF00u) |
               ((color & 0x00FF0000u) >> 16);
    }

    return color;
}

extern "C" StGpuU64 StarlixGpuPutPixel(
    const StarlixGpuHostApi* host,
    StGpuU32 x, StGpuU32 y, StGpuU32 color) {
    if (!StIntelGpuValidHost(host))
        return StGpuStatusInvalidArgument;

    if (x >= host->width || y >= host->height)
        return StGpuStatusOutOfBounds;

    const StGpuU64 index =
        static_cast<StGpuU64>(y) * host->pixels_per_scanline + x;
    const StGpuU64 capacity = host->framebuffer_size / 4;

    if (index >= capacity)
        return StGpuStatusOutOfBounds;

    volatile StGpuU32* framebuffer =
        reinterpret_cast<volatile StGpuU32*>(
            static_cast<StGpuU64>(host->framebuffer_base));

    framebuffer[index] = StIntelGpuConvertColor(host, color);
    return StGpuStatusSuccess;
}

extern "C" StGpuU64 StarlixGpuFillRect(
    const StarlixGpuHostApi* host,
    StGpuU32 x, StGpuU32 y,
    StGpuU32 width, StGpuU32 height, StGpuU32 color) {
    if (!StIntelGpuValidHost(host))
        return StGpuStatusInvalidArgument;

    if (!width || !height)
        return StGpuStatusSuccess;

    if (x >= host->width || y >= host->height)
        return StGpuStatusOutOfBounds;

    const StGpuU64 rightCandidate =
        static_cast<StGpuU64>(x) + width;
    const StGpuU64 bottomCandidate =
        static_cast<StGpuU64>(y) + height;

    const StGpuU32 right = static_cast<StGpuU32>(
        rightCandidate > host->width ? host->width : rightCandidate);
    const StGpuU32 bottom = static_cast<StGpuU32>(
        bottomCandidate > host->height ? host->height : bottomCandidate);

    volatile StGpuU32* framebuffer =
        reinterpret_cast<volatile StGpuU32*>(
            static_cast<StGpuU64>(host->framebuffer_base));

    const StGpuU32 pixel = StIntelGpuConvertColor(host, color);
    const StGpuU64 capacity = host->framebuffer_size / 4;

    for (StGpuU32 py = y; py < bottom; ++py) {
        const StGpuU64 row =
            static_cast<StGpuU64>(py) * host->pixels_per_scanline;

        for (StGpuU32 px = x; px < right; ++px) {
            const StGpuU64 index = row + px;
            if (index >= capacity)
                return StGpuStatusOutOfBounds;

            framebuffer[index] = pixel;
        }
    }

    return StGpuStatusSuccess;
}

extern "C" StGpuU64 StarlixGpuClear(
    const StarlixGpuHostApi* host, StGpuU32 color) {
    if (!StIntelGpuValidHost(host))
        return StGpuStatusInvalidArgument;

    return StarlixGpuFillRect(
        host, 0, 0, host->width, host->height, color);
}

extern "C" StGpuU64 StarlixGpuDriverEntry(
    const StarlixGpuHostApi* host,
    StarlixGpuDriverApi* api) {
    if (!StIntelGpuValidHost(host) || !api ||
        api->struct_size < sizeof(StarlixGpuDriverApi))
        return StGpuStatusInvalidArgument;

    api->abi_major = 1;
    api->abi_minor = 0;
    api->reserved = 0;

    api->capabilities =
        StGpuCapabilityPixel |
        StGpuCapabilityFillRect |
        StGpuCapabilityClear;

    api->put_pixel = &StarlixGpuPutPixel;
    api->fill_rect = &StarlixGpuFillRect;
    api->clear = &StarlixGpuClear;

    return StGpuStatusSuccess;
}


// ---------------------------------------------------------------------------
// Phase 2: caller-owned software backbuffer, frame presentation and animation.
// No physical Intel registers or GPU command submission are used here.
// ---------------------------------------------------------------------------

static bool StIntelGpuValidBackbuffer(
    const StarlixGpuBackbuffer* buffer) {
    if (!buffer ||
        buffer->struct_size < sizeof(StarlixGpuBackbuffer) ||
        buffer->abi_major != 2)
        return false;

    if (!buffer->base ||
        (buffer->base & 3ULL) != 0 ||
        buffer->size < 4 ||
        !buffer->width || !buffer->height ||
        !buffer->pixels_per_scanline ||
        buffer->pixels_per_scanline < buffer->width)
        return false;

    if (buffer->pixel_format != StGpuPixelFormatRgbReserved8 &&
        buffer->pixel_format != StGpuPixelFormatBgrReserved8)
        return false;

    const StGpuU64 capacity = buffer->size / 4;
    if (buffer->height > capacity / buffer->pixels_per_scanline)
        return false;

    const StGpuU64 maxValue = ~static_cast<StGpuU64>(0);
    if (buffer->base > maxValue - buffer->size)
        return false;

    return true;
}

static bool StIntelGpuRangesOverlap(
    StGpuU64 a, StGpuU64 aSize,
    StGpuU64 b, StGpuU64 bSize) {
    // Callers validate address + size overflow before reaching this helper.
    return a < b + bSize && b < a + aSize;
}

static StGpuU32 StIntelGpuConvertFrameColor(
    StGpuU32 pixelFormat, StGpuU32 color) {
    color &= 0x00FFFFFFu;

    if (pixelFormat == StGpuPixelFormatRgbReserved8) {
        return ((color & 0x000000FFu) << 16) |
               (color & 0x0000FF00u) |
               ((color & 0x00FF0000u) >> 16);
    }

    return color;
}

extern "C" StGpuU64 StarlixGpuFramePutPixel(
    StarlixGpuBackbuffer* buffer,
    StGpuU32 x, StGpuU32 y, StGpuU32 color) {
    if (!StIntelGpuValidBackbuffer(buffer))
        return StGpuStatusInvalidArgument;

    if (x >= buffer->width || y >= buffer->height)
        return StGpuStatusOutOfBounds;

    const StGpuU64 index =
        static_cast<StGpuU64>(y) * buffer->pixels_per_scanline + x;
    if (index >= buffer->size / 4)
        return StGpuStatusOutOfBounds;

    StGpuU32* pixels =
        reinterpret_cast<StGpuU32*>(buffer->base);
    pixels[index] =
        StIntelGpuConvertFrameColor(buffer->pixel_format, color);

    return StGpuStatusSuccess;
}

extern "C" StGpuU64 StarlixGpuFrameFillRect(
    StarlixGpuBackbuffer* buffer,
    StGpuU32 x, StGpuU32 y,
    StGpuU32 width, StGpuU32 height, StGpuU32 color) {
    if (!StIntelGpuValidBackbuffer(buffer))
        return StGpuStatusInvalidArgument;

    if (!width || !height)
        return StGpuStatusSuccess;

    if (x >= buffer->width || y >= buffer->height)
        return StGpuStatusOutOfBounds;

    const StGpuU64 rightCandidate =
        static_cast<StGpuU64>(x) + width;
    const StGpuU64 bottomCandidate =
        static_cast<StGpuU64>(y) + height;

    const StGpuU32 right = static_cast<StGpuU32>(
        rightCandidate > buffer->width ? buffer->width : rightCandidate);
    const StGpuU32 bottom = static_cast<StGpuU32>(
        bottomCandidate > buffer->height ? buffer->height : bottomCandidate);

    StGpuU32* pixels =
        reinterpret_cast<StGpuU32*>(buffer->base);
    const StGpuU32 pixel =
        StIntelGpuConvertFrameColor(buffer->pixel_format, color);
    const StGpuU64 capacity = buffer->size / 4;

    for (StGpuU32 py = y; py < bottom; ++py) {
        const StGpuU64 row =
            static_cast<StGpuU64>(py) * buffer->pixels_per_scanline;

        for (StGpuU32 px = x; px < right; ++px) {
            const StGpuU64 index = row + px;
            if (index >= capacity)
                return StGpuStatusOutOfBounds;
            pixels[index] = pixel;
        }
    }

    return StGpuStatusSuccess;
}

extern "C" StGpuU64 StarlixGpuFrameClear(
    StarlixGpuBackbuffer* buffer, StGpuU32 color) {
    if (!StIntelGpuValidBackbuffer(buffer))
        return StGpuStatusInvalidArgument;

    return StarlixGpuFrameFillRect(
        buffer, 0, 0, buffer->width, buffer->height, color);
}

extern "C" StGpuU64 StarlixGpuFramePresent(
    const StarlixGpuHostApi* host,
    const StarlixGpuBackbuffer* buffer) {
    if (!StIntelGpuValidHost(host) ||
        !StIntelGpuValidBackbuffer(buffer))
        return StGpuStatusInvalidArgument;

    if (host->width != buffer->width ||
        host->height != buffer->height ||
        host->pixel_format != buffer->pixel_format)
        return StGpuStatusInvalidArgument;

    // Double buffering requires independent source and destination ranges.
    if (StIntelGpuRangesOverlap(
            host->framebuffer_base, host->framebuffer_size,
            buffer->base, buffer->size))
        return StGpuStatusInvalidArgument;

    const StGpuU32* source =
        reinterpret_cast<const StGpuU32*>(buffer->base);
    volatile StGpuU32* destination =
        reinterpret_cast<volatile StGpuU32*>(
            static_cast<StGpuU64>(host->framebuffer_base));

    const StGpuU64 sourceCapacity = buffer->size / 4;
    const StGpuU64 destinationCapacity = host->framebuffer_size / 4;

    // Publish completed backbuffer writes before copying to the device-visible
    // framebuffer. This is an ordering fence, not a hardware VSync operation.
    __atomic_thread_fence(__ATOMIC_RELEASE);

    for (StGpuU32 y = 0; y < host->height; ++y) {
        const StGpuU64 srcRow =
            static_cast<StGpuU64>(y) * buffer->pixels_per_scanline;
        const StGpuU64 dstRow =
            static_cast<StGpuU64>(y) * host->pixels_per_scanline;

        for (StGpuU32 x = 0; x < host->width; ++x) {
            const StGpuU64 srcIndex = srcRow + x;
            const StGpuU64 dstIndex = dstRow + x;

            if (srcIndex >= sourceCapacity ||
                dstIndex >= destinationCapacity)
                return StGpuStatusOutOfBounds;

            destination[dstIndex] = source[srcIndex];
        }
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return StGpuStatusSuccess;
}

extern "C" StGpuU64 StarlixGpuAnimateRect(
    StarlixGpuBackbuffer* buffer,
    const StarlixGpuAnimation* animation,
    StGpuU64 nowMs) {
    if (!StIntelGpuValidBackbuffer(buffer) ||
        !animation ||
        animation->struct_size < sizeof(StarlixGpuAnimation))
        return StGpuStatusInvalidArgument;

    // Bound arithmetic and prevent unbounded animation requests.
    if (!animation->width || !animation->height ||
        !animation->duration_ms ||
        animation->duration_ms > 600000ULL ||
        animation->start_x >= buffer->width ||
        animation->end_x >= buffer->width ||
        animation->start_y >= buffer->height ||
        animation->end_y >= buffer->height)
        return StGpuStatusInvalidArgument;

    StGpuU64 elapsed = 0;
    if (nowMs > animation->start_time_ms)
        elapsed = nowMs - animation->start_time_ms;

    if (elapsed > animation->duration_ms)
        elapsed = animation->duration_ms;

    const long long dx =
        static_cast<long long>(animation->end_x) -
        static_cast<long long>(animation->start_x);
    const long long dy =
        static_cast<long long>(animation->end_y) -
        static_cast<long long>(animation->start_y);

    const long long x =
        static_cast<long long>(animation->start_x) +
        (dx * static_cast<long long>(elapsed)) /
            static_cast<long long>(animation->duration_ms);

    const long long y =
        static_cast<long long>(animation->start_y) +
        (dy * static_cast<long long>(elapsed)) /
            static_cast<long long>(animation->duration_ms);

    // The caller redraws the scene/backbuffer each frame before invoking this
    // helper, so old animation positions do not leave trails.
    return StarlixGpuFrameFillRect(
        buffer,
        static_cast<StGpuU32>(x),
        static_cast<StGpuU32>(y),
        animation->width,
        animation->height,
        animation->color);
}

extern "C" StGpuU64 StarlixGpuFrameDriverEntry(
    const StarlixGpuHostApi* host,
    StarlixGpuFrameDriverApi* api) {
    if (!StIntelGpuValidHost(host) || !api ||
        api->struct_size < sizeof(StarlixGpuFrameDriverApi))
        return StGpuStatusInvalidArgument;

    api->abi_major = 2;
    api->abi_minor = 0;
    api->reserved = 0;
    api->capabilities =
        StGpuCapabilityPixel |
        StGpuCapabilityFillRect |
        StGpuCapabilityClear |
        StGpuCapabilityFramePresent |
        StGpuCapabilityFrameAnimation;

    api->put_pixel = &StarlixGpuFramePutPixel;
    api->fill_rect = &StarlixGpuFrameFillRect;
    api->clear = &StarlixGpuFrameClear;
    api->present = &StarlixGpuFramePresent;
    api->animate_rect = &StarlixGpuAnimateRect;

    return StGpuStatusSuccess;
}

// External KEXT entry. It validates the generic PCI snapshot supplied by the
// kernel, then exposes the existing GOP software graphics backend. It does
// not read BAR memory, write PCI configuration, map MMIO or enable acceleration.
extern "C" StGpuU64 StarlixGpuKextEntry(
    const StarlixGpuKextHostApiV1* host,
    StarlixGpuKextDriverApiV1* api) {
    if (!host || !api ||
        host->struct_size < sizeof(StarlixGpuKextHostApiV1) ||
        api->struct_size < sizeof(StarlixGpuKextDriverApiV1) ||
        !host->pci_device || !host->graphics_host || !host->pci_resources)
        return StarlixGpuKextInvalidArgumentV1;

    if (host->abi_major != STARLIX_GPU_KEXT_ABI_MAJOR ||
        host->abi_minor > STARLIX_GPU_KEXT_ABI_MINOR ||
        host->pci_resources->struct_size < sizeof(StarlixPciResourceHostApiV1) ||
        host->pci_resources->abi_major != STARLIX_PCI_RESOURCE_ABI_MAJOR ||
        host->pci_resources->abi_minor > STARLIX_PCI_RESOURCE_ABI_MINOR ||
        !host->pci_resources->claim_bar ||
        !host->pci_resources->release_resource)
        return StarlixGpuKextAbiMismatchV1;

    if (!starlix::drivers::intel_gpu::IsSupportedPciDevice(host->pci_device))
        return StarlixGpuKextUnsupportedDeviceV1;

    StarlixPciResourceV1 bar0{};
    if (!host->pci_resources->claim_bar(host->pci_resources->context,
            host->pci_device, 0u, &bar0) ||
        !StarlixPciResourceIsValidV1(&bar0) ||
        bar0.bus != host->pci_device->bus ||
        bar0.device != host->pci_device->device ||
        bar0.function != host->pci_device->function ||
        bar0.bar_index != 0u ||
        bar0.base != host->pci_device->bars[0].base) {
        if (bar0.ownership_token != 0u)
            (void)host->pci_resources->release_resource(
                host->pci_resources->context, bar0.ownership_token);
        return StarlixGpuKextBar0UnavailableV1;
    }

    api->abi_major = STARLIX_GPU_KEXT_ABI_MAJOR;
    api->abi_minor = STARLIX_GPU_KEXT_ABI_MINOR;
    api->flags = StarlixGpuKextPciMatchedV1;
    api->capabilities = StarlixGpuKextCapabilityGopFramebufferV1;
    api->pci_vendor_id = host->pci_device->vendor_id;
    api->pci_device_id = host->pci_device->device_id;
    api->pci_bus = host->pci_device->bus;
    api->pci_device = host->pci_device->device;
    api->pci_function = host->pci_device->function;
    api->reserved = 0u;
    api->bar0_resource = bar0;

    api->graphics = StarlixGpuFrameDriverApi{};
    api->graphics.struct_size = sizeof(StarlixGpuFrameDriverApi);
    const StGpuU64 graphics_status =
        StarlixGpuFrameDriverEntry(host->graphics_host, &api->graphics);
    if (graphics_status != StGpuStatusSuccess) {
        (void)host->pci_resources->release_resource(
            host->pci_resources->context, bar0.ownership_token);
        api->flags = 0u;
        api->capabilities = 0u;
        api->bar0_resource = StarlixPciResourceV1{};
        return StarlixGpuKextGraphicsInitFailedV1;
    }

    api->flags |= StarlixGpuKextGopFramebufferReadyV1;
    return StarlixGpuKextSuccessV1;
}
