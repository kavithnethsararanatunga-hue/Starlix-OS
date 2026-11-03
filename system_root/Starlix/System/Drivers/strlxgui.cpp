// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>

extern "C" {
    uint64_t StarlixSyscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
}

// Draw Rect via Syscall (Assume Syscall 10 = DrawRect)
void DrawRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    StarlixSyscall(10, x, y, w, h, color);
}

// Exported Driver Functions
extern "C" void StDrvDrawWindow(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const char* title) {
    DrawRect(x + 6, y + 6, width, height, 0x00020617); // Shadow
    DrawRect(x - 1, y - 1, width + 2, height + 2, 0x00475569); // Border
    DrawRect(x, y, width, height, 0x00F8FAFC); // Background
    DrawRect(x, y, width, 32, 0x000F172A); // Titlebar
    DrawRect(x + width - 36, y + 4, 28, 24, 0x00EF4444); // Close Button
}

extern "C" void StDrvDrawTaskbar(uint32_t screen_w, uint32_t screen_h) {
    uint32_t tb_h = 48;
    uint32_t tb_y = screen_h - tb_h;
    DrawRect(0, tb_y, screen_w, tb_h, 0x001E293B); // Taskbar Background
    DrawRect(0, tb_y, screen_w, 1, 0x00334155); // Top Border

    uint32_t start_x = (screen_w / 2) - 16;
    DrawRect(start_x, tb_y + 8, 32, 32, 0x0038BDF8); // Start Button
}
