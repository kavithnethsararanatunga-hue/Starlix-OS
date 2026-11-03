#include <stdint.h>
#include <stdio.h>

#include "driver/graphics_abi_v2.hpp"

static int Fail(const char* message) {
    printf("FAIL: %s\n", message);
    return 1;
}

int main() {
    struct GuardedPixels {
        uint32_t before;
        uint32_t pixels[12];
        uint32_t after;
    } display{}, back{};

    display.before = 0xAABBCCDDu;
    display.after = 0x11223344u;
    back.before = 0x55667788u;
    back.after = 0x99AABBCCu;

    StarlixGpuHostApi host{};
    host.struct_size = sizeof(host);
    host.abi_major = 1;
    host.pixel_format = StGpuPixelFormatBgrReserved8;
    host.framebuffer_base =
        reinterpret_cast<StGpuU64>(&display.pixels[0]);
    host.framebuffer_size = sizeof(display.pixels);
    host.width = 4;
    host.height = 3;
    host.pixels_per_scanline = 4;

    StarlixGpuFrameDriverApi api{};
    api.struct_size = sizeof(api);

    if (StarlixGpuFrameDriverEntry(&host, &api) != StGpuStatusSuccess)
        return Fail("v2 frame-driver entry rejected valid host");

    if (api.abi_major != 2 || !api.put_pixel ||
        !api.fill_rect || !api.clear ||
        !api.present || !api.animate_rect)
        return Fail("v2 ABI functions or version missing");

    if ((api.capabilities &
        (StGpuCapabilityFramePresent |
         StGpuCapabilityFrameAnimation)) !=
        (StGpuCapabilityFramePresent |
         StGpuCapabilityFrameAnimation))
        return Fail("frame capabilities missing");

    StarlixGpuBackbuffer buffer{};
    buffer.struct_size = sizeof(buffer);
    buffer.abi_major = 2;
    buffer.pixel_format = StGpuPixelFormatBgrReserved8;
    buffer.base = reinterpret_cast<StGpuU64>(&back.pixels[0]);
    buffer.size = sizeof(back.pixels);
    buffer.width = 4;
    buffer.height = 3;
    buffer.pixels_per_scanline = 4;

    if (api.clear(&buffer, 0x00112233u) != StGpuStatusSuccess)
        return Fail("backbuffer clear failed");

    for (unsigned i = 0; i < 12; ++i) {
        if (back.pixels[i] != 0x00112233u)
            return Fail("backbuffer clear pixel mismatch");
    }

    if (api.fill_rect(&buffer, 2, 1, 20, 20, 0x0000AA55u)
        != StGpuStatusSuccess)
        return Fail("backbuffer clipped rectangle failed");

    if (back.pixels[6] != 0x0000AA55u ||
        back.pixels[7] != 0x0000AA55u ||
        back.pixels[10] != 0x0000AA55u ||
        back.pixels[11] != 0x0000AA55u)
        return Fail("backbuffer rectangle pixels incorrect");

    if (api.present(&host, &buffer) != StGpuStatusSuccess)
        return Fail("backbuffer presentation failed");

    for (unsigned i = 0; i < 12; ++i) {
        if (display.pixels[i] != back.pixels[i])
            return Fail("presented framebuffer differs from backbuffer");
    }

    // The two ranges must never alias.
    StarlixGpuBackbuffer overlapping = buffer;
    overlapping.base = host.framebuffer_base;
    if (api.present(&host, &overlapping) != StGpuStatusInvalidArgument)
        return Fail("overlapping front/back buffers were not rejected");

    // Geometry mismatch must fail closed.
    StarlixGpuBackbuffer wrongSize = buffer;
    wrongSize.width = 3;
    if (api.present(&host, &wrongSize) != StGpuStatusInvalidArgument)
        return Fail("mismatched frame dimensions were not rejected");

    // Midpoint animation: (0,0) -> (2,0), elapsed 50/100 ms => (1,0).
    if (api.clear(&buffer, 0) != StGpuStatusSuccess)
        return Fail("animation test clear failed");

    StarlixGpuAnimation animation{};
    animation.struct_size = sizeof(animation);
    animation.start_x = 0;
    animation.start_y = 0;
    animation.end_x = 2;
    animation.end_y = 0;
    animation.width = 1;
    animation.height = 1;
    animation.color = 0x00010203u;
    animation.start_time_ms = 100;
    animation.duration_ms = 100;

    if (api.animate_rect(&buffer, &animation, 150) !=
        StGpuStatusSuccess)
        return Fail("animation midpoint update failed");

    if (back.pixels[1] != 0x00010203u ||
        back.pixels[0] != 0 ||
        back.pixels[2] != 0)
        return Fail("animation midpoint position incorrect");

    if (api.present(&host, &buffer) != StGpuStatusSuccess ||
        display.pixels[1] != 0x00010203u)
        return Fail("animated frame presentation failed");

    if (api.clear(&buffer, 0) != StGpuStatusSuccess)
        return Fail("animation end clear failed");

    if (api.animate_rect(&buffer, &animation, 250) !=
        StGpuStatusSuccess)
        return Fail("animation final update failed");

    if (back.pixels[2] != 0x00010203u)
        return Fail("animation did not clamp to final position");

    StarlixGpuAnimation invalidAnimation = animation;
    invalidAnimation.duration_ms = 0;
    if (api.animate_rect(&buffer, &invalidAnimation, 150) !=
        StGpuStatusInvalidArgument)
        return Fail("zero-duration animation was not rejected");

    invalidAnimation = animation;
    invalidAnimation.duration_ms = 600001;
    if (api.animate_rect(&buffer, &invalidAnimation, 150) !=
        StGpuStatusInvalidArgument)
        return Fail("unbounded animation duration was not rejected");

    StarlixGpuBackbuffer invalidBuffer = buffer;
    invalidBuffer.pixels_per_scanline = 2;
    if (api.clear(&invalidBuffer, 0) != StGpuStatusInvalidArgument)
        return Fail("invalid backbuffer stride was not rejected");

    if (display.before != 0xAABBCCDDu ||
        display.after != 0x11223344u ||
        back.before != 0x55667788u ||
        back.after != 0x99AABBCCu)
        return Fail("front/back buffer guard overwritten");

    printf("PASS: graphics ABI v2 initialization\n");
    printf("PASS: caller-owned backbuffer clear and rectangle drawing\n");
    printf("PASS: separate backbuffer to GOP framebuffer presentation\n");
    printf("PASS: overlapping and mismatched buffers rejected\n");
    printf("PASS: animation interpolation at midpoint and endpoint\n");
    printf("PASS: animation duration and buffer-stride validation\n");
    printf("PASS: front/back framebuffer guard protection\n");
    printf("NOTE: software host tests only; no GPU registers or VSync accessed.\n");
    return 0;
}
