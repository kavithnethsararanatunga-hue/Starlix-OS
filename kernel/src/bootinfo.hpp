// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

enum StarlixFramebufferPixelFormat : unsigned int {
    StarlixFramebufferPixelFormatUnknown = 0,
    StarlixFramebufferPixelFormatRgbReserved8 = 1,
    StarlixFramebufferPixelFormatBgrReserved8 = 2,
    StarlixFramebufferPixelFormatBitMask = 3,
    StarlixFramebufferPixelFormatBltOnly = 4
};

enum StarlixBootModuleKind : unsigned int {
    StarlixBootModuleKindUnknown = 0,
    StarlixBootModuleKindDriverSys = 1,
    StarlixBootModuleKindLibraryLbr = 2,
    StarlixBootModuleKindKextDb = 3 // STARLIX_PHASE13_KEXT
};

struct StarlixBootModule {
    char Path[320]; // STARLIX_PHASE13_KEXT: holds full APFS bundle path
    unsigned long long ImageBase;
    unsigned long long ImageSize;
    unsigned int Kind;
    unsigned int Flags;
};

struct StarlixBootInfo {
    unsigned long long FramebufferBase;
    unsigned long long FramebufferSize;
    unsigned int        FramebufferWidth;
    unsigned int        FramebufferHeight;
    unsigned int        FramebufferPixelsPerScanLine;
    unsigned int        FramebufferPixelFormat;

    unsigned long long MemoryMapBase;
    unsigned long long MemoryMapSize;
    unsigned long long MemoryMapDescriptorSize;
    unsigned int        MemoryMapDescriptorVersion;
    unsigned int        Reserved1;

    // External module inventory and payloads allocated by the UEFI loader.
    unsigned long long SystemModuleTableBase;
    unsigned int        SystemModuleCount;
    unsigned int        SystemModuleDescriptorSize;
};
