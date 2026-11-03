#include <stdint.h>
#include <stdio.h>

#include "driver/graphics_abi.hpp"

extern "C" StGpuU64 StarlixIntelGpuMatchesPciId(
    StGpuU32, StGpuU32);

static int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

int main() {
    if (!StarlixIntelGpuMatchesPciId(0x8086, 0x0152))
        return Fail("Intel HD 2500 PCI ID not matched");

    if (StarlixIntelGpuMatchesPciId(0x8086, 0x0162))
        return Fail("Intel HD 4000 incorrectly matched");

    if (StarlixIntelGpuMatchesPciId(0x10DE, 0x0152))
        return Fail("non-Intel vendor incorrectly matched");

    struct GuardedFramebuffer {
        uint32_t before;
        uint32_t pixels[12];
        uint32_t after;
    } guarded{};

    guarded.before = 0xAABBCCDD;
    guarded.after = 0x11223344;

    StarlixGpuHostApi host{};
    host.struct_size = sizeof(host);
    host.abi_major = 1;
    host.abi_minor = 0;
    host.pixel_format = StGpuPixelFormatBgrReserved8;
    host.framebuffer_base =
        reinterpret_cast<StGpuU64>(&guarded.pixels[0]);
    host.framebuffer_size = sizeof(guarded.pixels);
    host.width = 4;
    host.height = 3;
    host.pixels_per_scanline = 4;

    StarlixGpuDriverApi api{};
    api.struct_size = sizeof(api);

    if (StarlixGpuDriverEntry(&host, &api) != StGpuStatusSuccess)
        return Fail("driver entry rejected valid framebuffer");

    if (!api.put_pixel || !api.fill_rect || !api.clear)
        return Fail("driver API function missing");

    if ((api.capabilities &
        (StGpuCapabilityPixel |
         StGpuCapabilityFillRect |
         StGpuCapabilityClear)) !=
        (StGpuCapabilityPixel |
         StGpuCapabilityFillRect |
         StGpuCapabilityClear))
        return Fail("basic graphics capabilities missing");

    if (api.clear(&host, 0x00112233) != StGpuStatusSuccess)
        return Fail("framebuffer clear failed");

    for (unsigned i = 0; i < 12; ++i) {
        if (guarded.pixels[i] != 0x00112233)
            return Fail("clear produced incorrect pixel");
    }

    if (api.fill_rect(&host, 2, 1, 20, 20, 0x0000AA55)
        != StGpuStatusSuccess)
        return Fail("clipped rectangle failed");

    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const uint32_t expected =
                (y >= 1 && x >= 2) ? 0x0000AA55 : 0x00112233;
            if (guarded.pixels[y * 4 + x] != expected)
                return Fail("rectangle clipping/pixels incorrect");
        }
    }

    if (api.put_pixel(&host, 3, 0, 0x00010203)
        != StGpuStatusSuccess)
        return Fail("pixel drawing failed");

    if (guarded.pixels[3] != 0x00010203)
        return Fail("BGR pixel byte ordering incorrect");

    if (api.put_pixel(&host, 4, 0, 0) != StGpuStatusOutOfBounds)
        return Fail("out-of-bounds pixel was not rejected");

    if (guarded.before != 0xAABBCCDD ||
        guarded.after != 0x11223344)
        return Fail("framebuffer guard overwritten");

    host.pixel_format = StGpuPixelFormatRgbReserved8;
    if (api.put_pixel(&host, 0, 0, 0x00112233)
        != StGpuStatusSuccess)
        return Fail("RGB pixel drawing failed");

    if (guarded.pixels[0] != 0x00332211)
        return Fail("RGB byte ordering incorrect");

    host.pixel_format = StGpuPixelFormatUnknown;
    if (api.put_pixel(&host, 0, 0, 0) !=
        StGpuStatusInvalidArgument)
        return Fail("unsupported pixel format was not rejected");

    printf("PASS: Intel HD 2500 PCI identity match\n");
    printf("PASS: driver ABI v1 entry and capabilities\n");
    printf("PASS: framebuffer clear and pixel drawing\n");
    printf("PASS: clipped rectangle drawing\n");
    printf("PASS: RGB/BGR pixel conversion\n");
    printf("PASS: framebuffer bounds and guard protection\n");
    printf("NOTE: host tests only; no physical GPU registers accessed.\n");
    return 0;
}
