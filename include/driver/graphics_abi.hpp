#pragma once

typedef unsigned int StGpuU32;
typedef unsigned long long StGpuU64;

enum StarlixGpuPixelFormat : StGpuU32 {
    StGpuPixelFormatUnknown = 0,
    StGpuPixelFormatRgbReserved8 = 1,
    StGpuPixelFormatBgrReserved8 = 2
};

enum StarlixGpuStatus : StGpuU64 {
    StGpuStatusSuccess = 0,
    StGpuStatusInvalidArgument = 1,
    StGpuStatusUnsupportedFormat = 2,
    StGpuStatusOutOfBounds = 3
};

enum StarlixGpuCapabilities : StGpuU64 {
    StGpuCapabilityPixel = 1ULL << 0,
    StGpuCapabilityFillRect = 1ULL << 1,
    StGpuCapabilityClear = 1ULL << 2
};

struct StarlixGpuHostApi;

typedef StGpuU64 (*StarlixGpuPutPixelFn)(
    const StarlixGpuHostApi*, StGpuU32, StGpuU32, StGpuU32);
typedef StGpuU64 (*StarlixGpuFillRectFn)(
    const StarlixGpuHostApi*, StGpuU32, StGpuU32,
    StGpuU32, StGpuU32, StGpuU32);
typedef StGpuU64 (*StarlixGpuClearFn)(
    const StarlixGpuHostApi*, StGpuU32);

struct StarlixGpuHostApi {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 pixel_format;

    StGpuU64 framebuffer_base;
    StGpuU64 framebuffer_size;

    StGpuU32 width;
    StGpuU32 height;
    StGpuU32 pixels_per_scanline;
    StGpuU32 reserved;
};

struct StarlixGpuDriverApi {
    StGpuU32 struct_size;
    StGpuU32 abi_major;
    StGpuU32 abi_minor;
    StGpuU32 reserved;

    StGpuU64 capabilities;
    StarlixGpuPutPixelFn put_pixel;
    StarlixGpuFillRectFn fill_rect;
    StarlixGpuClearFn clear;
};

#ifdef __cplusplus
extern "C" {
#endif

StGpuU64 StarlixGpuPutPixel(
    const StarlixGpuHostApi*, StGpuU32, StGpuU32, StGpuU32);
StGpuU64 StarlixGpuFillRect(
    const StarlixGpuHostApi*, StGpuU32, StGpuU32,
    StGpuU32, StGpuU32, StGpuU32);
StGpuU64 StarlixGpuClear(
    const StarlixGpuHostApi*, StGpuU32);
StGpuU64 StarlixGpuDriverEntry(
    const StarlixGpuHostApi*, StarlixGpuDriverApi*);

#ifdef __cplusplus
}
#endif
