// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "framebuffer.hpp"
#include "framebuffer_policy.hpp"
#include "../cpu/idt.hpp"

namespace starlix::drivers::framebuffer {
namespace {
StarlixBootInfo* g_boot_info = nullptr;

bool valid_info(const StarlixBootInfo* info) {
    return info && policy::valid_geometry(
        info->FramebufferBase, info->FramebufferSize,
        info->FramebufferWidth, info->FramebufferHeight,
        info->FramebufferPixelsPerScanLine, info->FramebufferPixelFormat,
        StarlixFramebufferPixelFormatRgbReserved8,
        StarlixFramebufferPixelFormatBgrReserved8);
}

unsigned int convert_color(const StarlixBootInfo* info, unsigned int color) {
    if (info && info->FramebufferPixelFormat ==
                    StarlixFramebufferPixelFormatRgbReserved8) {
        return ((color & 0x000000FFu) << 16) |
               (color & 0x0000FF00u) |
               ((color & 0x00FF0000u) >> 16);
    }
    return color;
}
} // namespace

bool init(StarlixBootInfo* boot_info) {
    g_boot_info = nullptr;
    if (!valid_info(boot_info)) return false;
    g_boot_info = boot_info;
    return true;
}

bool ready() { return valid_info(g_boot_info); }

bool query(FramebufferInfo& out_info) {
    out_info = FramebufferInfo{};
    if (!ready()) return false;
    out_info.struct_size = sizeof(FramebufferInfo);
    out_info.width = g_boot_info->FramebufferWidth;
    out_info.height = g_boot_info->FramebufferHeight;
    out_info.pixels_per_scanline = g_boot_info->FramebufferPixelsPerScanLine;
    out_info.pixel_format = g_boot_info->FramebufferPixelFormat;
    return true;
}

bool blit(unsigned int x, unsigned int y,
          unsigned int width, unsigned int height,
          unsigned int source_stride,
          const unsigned int* source_pixels) {
    if (!ready() || !source_pixels || !policy::valid_blit(
            x, y, width, height, source_stride,
            g_boot_info->FramebufferWidth, g_boot_info->FramebufferHeight))
        return false;

    auto* destination = reinterpret_cast<volatile unsigned int*>(
        static_cast<unsigned long long>(g_boot_info->FramebufferBase));
    const unsigned long long capacity =
        g_boot_info->FramebufferSize / sizeof(unsigned int);
    for (unsigned int row = 0u; row < height; ++row) {
        const unsigned long long destination_index =
            static_cast<unsigned long long>(y + row) *
                g_boot_info->FramebufferPixelsPerScanLine + x;
        if (destination_index > capacity ||
            width > capacity - destination_index) return false;
        for (unsigned int column = 0u; column < width; ++column) {
            destination[destination_index + column] =
                convert_color(g_boot_info,
                    source_pixels[static_cast<unsigned long long>(row) *
                                  source_stride + column]);
        }
    }
    return true;
}

bool fade_to_black(unsigned int duration_ms) {
    /* Validate every prerequisite before the first pixel is modified. A failed
       clock/IF check leaves the inherited UEFI splash intact. */
    if (!ready() || duration_ms == 0u || duration_ms > 5000u ||
        !starlix::cpu::timer_is_ready() ||
        !starlix::cpu::interrupts_enabled()) return false;

    auto* destination = reinterpret_cast<volatile unsigned int*>(
        static_cast<unsigned long long>(g_boot_info->FramebufferBase));
    const unsigned int width = g_boot_info->FramebufferWidth;
    const unsigned int height = g_boot_info->FramebufferHeight;
    const unsigned int pitch = g_boot_info->FramebufferPixelsPerScanLine;
    const unsigned long long capacity =
        g_boot_info->FramebufferSize / sizeof(unsigned int);

    constexpr unsigned int FRAME_INTERVAL_MS = 33u;
    const unsigned int total_steps =
        (duration_ms + FRAME_INTERVAL_MS - 1u) / FRAME_INTERVAL_MS;
    if (total_steps == 0u) return false;

    const unsigned long long start_ms = starlix::cpu::timer_now_ms();
    unsigned int previous_remaining = 255u;
    for (unsigned int step = 1u; step <= total_steps; ++step) {
        const unsigned long long target_elapsed =
            (static_cast<unsigned long long>(step) * duration_ms) / total_steps;
        while (starlix::cpu::timer_now_ms() - start_ms < target_elapsed) {
            if (!starlix::cpu::timer_is_ready() ||
                !starlix::cpu::interrupts_enabled()) return false;
            asm volatile("hlt" ::: "memory");
        }

        const unsigned int remaining = policy::fade_remaining(step, total_steps);
        if (remaining >= previous_remaining) continue;
        for (unsigned int row = 0u; row < height; ++row) {
            const unsigned long long row_index =
                static_cast<unsigned long long>(row) * pitch;
            if (row_index > capacity || width > capacity - row_index) return false;
            for (unsigned int column = 0u; column < width; ++column) {
                const unsigned long long index = row_index + column;
                const unsigned int pixel = destination[index];
                const unsigned int red = policy::fade_scale_channel(
                    (pixel >> 16u) & 0xFFu, remaining, previous_remaining);
                const unsigned int green = policy::fade_scale_channel(
                    (pixel >> 8u) & 0xFFu, remaining, previous_remaining);
                const unsigned int blue = policy::fade_scale_channel(
                    pixel & 0xFFu, remaining, previous_remaining);
                destination[index] = (pixel & 0xFF000000u) |
                    (red << 16u) | (green << 8u) | blue;
            }
        }
        previous_remaining = remaining;
    }
    return previous_remaining == 0u;
}

} // namespace starlix::drivers::framebuffer
