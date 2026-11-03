// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::drivers::framebuffer::policy {
constexpr unsigned long long MAX_IDENTITY_END = 0x100000000ULL;
constexpr unsigned int MAX_WIDTH = 4096u;
constexpr unsigned int MAX_HEIGHT = 2160u;
constexpr unsigned int MAX_PIXELS_PER_BLIT = 4096u;

/* Fade policy is deliberately independent of timers and framebuffer access.
   step=0 represents the original image; step=total_steps is black. */
constexpr unsigned int fade_remaining(unsigned int step,
                                      unsigned int total_steps) {
    if (total_steps == 0u || step >= total_steps) return 0u;
    return static_cast<unsigned int>(
        (static_cast<unsigned long long>(total_steps - step) * 255ULL) /
        total_steps);
}

constexpr unsigned int fade_scale_channel(unsigned int channel,
                                          unsigned int next_remaining,
                                          unsigned int previous_remaining) {
    if (previous_remaining == 0u || next_remaining == 0u) return 0u;
    if (next_remaining >= previous_remaining) return channel;
    const unsigned long long scaled =
        static_cast<unsigned long long>(channel) * next_remaining +
        previous_remaining / 2u;
    return static_cast<unsigned int>(scaled / previous_remaining);
}

constexpr bool valid_geometry(unsigned long long base,
                              unsigned long long size,
                              unsigned int width,
                              unsigned int height,
                              unsigned int pitch,
                              unsigned int pixel_format,
                              unsigned int rgb_format,
                              unsigned int bgr_format) {
    if (base == 0ULL || size == 0ULL || base >= MAX_IDENTITY_END ||
        size > MAX_IDENTITY_END - base || width < 320u || width > MAX_WIDTH ||
        height < 240u || height > MAX_HEIGHT || pitch < width ||
        (pixel_format != rgb_format && pixel_format != bgr_format)) return false;
    const unsigned long long p = pitch;
    const unsigned long long h = height;
    if (p != 0ULL && h > (~0ULL / p)) return false;
    const unsigned long long pixels = p * h;
    if (pixels > (~0ULL / 4ULL)) return false;
    return pixels * 4ULL <= size;
}

constexpr bool valid_blit(unsigned int x, unsigned int y,
                          unsigned int width, unsigned int height,
                          unsigned int source_stride,
                          unsigned int screen_width,
                          unsigned int screen_height) {
    if (width == 0u || height == 0u || width > MAX_PIXELS_PER_BLIT ||
        height > MAX_PIXELS_PER_BLIT || source_stride < width ||
        source_stride > MAX_PIXELS_PER_BLIT || x > screen_width ||
        width > screen_width - x || y > screen_height ||
        height > screen_height - y) return false;
    const unsigned long long needed =
        static_cast<unsigned long long>(height - 1u) * source_stride + width;
    return needed <= MAX_PIXELS_PER_BLIT;
}
} // namespace starlix::drivers::framebuffer::policy
