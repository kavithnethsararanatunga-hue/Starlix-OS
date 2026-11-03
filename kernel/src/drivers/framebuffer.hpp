// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../bootinfo.hpp"

namespace starlix::drivers::framebuffer {

struct FramebufferInfo {
    unsigned int struct_size;
    unsigned int width;
    unsigned int height;
    unsigned int pixels_per_scanline;
    unsigned int pixel_format;
    unsigned int reserved;
};

bool init(StarlixBootInfo* boot_info);
bool ready();
bool query(FramebufferInfo& out_info);
bool blit(unsigned int x, unsigned int y,
          unsigned int width, unsigned int height,
          unsigned int source_stride,
          const unsigned int* source_pixels);
// Fade the inherited UEFI splash to black using the initialized PIT clock.
// Returns false without touching the framebuffer when timing/interrupt support
// is unavailable; this prevents a false-success or instantaneous fake fade.
bool fade_to_black(unsigned int duration_ms);

} // namespace starlix::drivers::framebuffer
