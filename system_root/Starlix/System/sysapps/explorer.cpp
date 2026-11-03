// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>

extern "C" {
    uint64_t StarlixDisplayString(const char* msg, int y);
    void StDrvDrawWindow(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char* title);
    void StDrvDrawTaskbar(uint32_t w, uint32_t h);
}

int main() {
    // Call Graphic Driver (.sys) functions from User Space!
    StDrvDrawTaskbar(1024, 768);
    StDrvDrawWindow(480, 80, 480, 300, "Starlix Desktop Shell");

    // Print text inside window
    StarlixDisplayString("Executing: /Starlix/System/sysapps/explorer.ect", 120);
    StarlixDisplayString("Status   : True Ring 3 User Mode Active", 150);
    StarlixDisplayString("Driver   : Linked to strlxgui.sys", 180);

    return 0;
}
