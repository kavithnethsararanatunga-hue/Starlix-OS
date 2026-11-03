#pragma once

#include "driver/graphics_abi.hpp"

// Starlix Graphics ABI v2.
// ABI v1 structures and entry points remain unchanged.
// All frame buffers are caller-owned; this API never allocates memory.

enum StarlixGpuFrameCapabilities : StGpuU64 {
    StGpuCapabilityFramePresent = 1ULL << 8,
    StGpuCapabilityFrameAnimation = 1ULL << 9
};

struct StarlixGpuBackbuffer {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 pixel_format;

    StGpuU64 base;
    StGpuU64 size;

    StGpuU32 width;
    StGpuU32 height;
    StGpuU32 pixels_per_scanline;
    StGpuU32 reserved;
};

struct StarlixGpuAnimation {
    StGpuU32 struct_size;
    StGpuU32 start_x;
    StGpuU32 start_y;
    StGpuU32 end_x;
    StGpuU32 end_y;
    StGpuU32 width;
    StGpuU32 height;
    StGpuU32 color;
    StGpuU32 reserved;

    StGpuU64 start_time_ms;
    StGpuU64 duration_ms;
};

typedef StGpuU64 (*StarlixGpuFramePutPixelFn)(
    StarlixGpuBackbuffer*, StGpuU32, StGpuU32, StGpuU32);

typedef StGpuU64 (*StarlixGpuFrameFillRectFn)(
    StarlixGpuBackbuffer*, StGpuU32, StGpuU32,
    StGpuU32, StGpuU32, StGpuU32);

typedef StGpuU64 (*StarlixGpuFrameClearFn)(
    StarlixGpuBackbuffer*, StGpuU32);

typedef StGpuU64 (*StarlixGpuFramePresentFn)(
    const StarlixGpuHostApi*, const StarlixGpuBackbuffer*);

typedef StGpuU64 (*StarlixGpuAnimateRectFn)(
    StarlixGpuBackbuffer*, const StarlixGpuAnimation*, StGpuU64);

struct StarlixGpuFrameDriverApi {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 reserved;

    StGpuU64 capabilities;

    StarlixGpuFramePutPixelFn put_pixel;
    StarlixGpuFrameFillRectFn fill_rect;
    StarlixGpuFrameClearFn clear;
    StarlixGpuFramePresentFn present;
    StarlixGpuAnimateRectFn animate_rect;
};

#ifdef __cplusplus
extern "C" {
#endif

StGpuU64 StarlixGpuFramePutPixel(
    StarlixGpuBackbuffer*, StGpuU32, StGpuU32, StGpuU32);

StGpuU64 StarlixGpuFrameFillRect(
    StarlixGpuBackbuffer*, StGpuU32, StGpuU32,
    StGpuU32, StGpuU32, StGpuU32);

StGpuU64 StarlixGpuFrameClear(
    StarlixGpuBackbuffer*, StGpuU32);

StGpuU64 StarlixGpuFramePresent(
    const StarlixGpuHostApi*, const StarlixGpuBackbuffer*);

StGpuU64 StarlixGpuAnimateRect(
    StarlixGpuBackbuffer*,
    const StarlixGpuAnimation*,
    StGpuU64 now_ms);

StGpuU64 StarlixGpuFrameDriverEntry(
    const StarlixGpuHostApi*,
    StarlixGpuFrameDriverApi*);

#ifdef __cplusplus
}
#endif
