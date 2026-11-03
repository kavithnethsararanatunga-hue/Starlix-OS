// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../bootinfo.hpp"

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::gui {

enum class InputEventType : uint8_t { KeyDown = 1u, KeyUp = 2u, MouseReport = 3u };
struct InputEvent {
    InputEventType type;
    uint8_t code;
    uint8_t modifiers;
    uint8_t buttons;
    int16_t dx;
    int16_t dy;
    int8_t wheel;
};

void gui_init(StarlixBootInfo* bootInfo);
void gui_handle_input(const InputEvent* event);
void DrawLockScreen(const char* pass_buffer);
void DrawDesktopShell();
void DrawWindow(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const char* title);
void DrawWindowText(uint32_t win_x, uint32_t win_y, uint32_t rel_x, uint32_t rel_y, const char* text, uint32_t color);
void DrawMouseCursor(uint32_t x, uint32_t y);

} // namespace starlix::gui
