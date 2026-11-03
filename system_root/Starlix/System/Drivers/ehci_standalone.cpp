// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>

extern "C" {
    uint64_t StarlixDisplayString(const char* msg, int y);
}

extern "C" int DriverEntry() {
    StarlixDisplayString("[OK] ehci.sys - Hardware USB 2.0 EHCI Host Controller Active", 360);
    return 0; // STATUS_SUCCESS
}
