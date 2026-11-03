// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "window.hpp"
#include "keyboard_text.hpp"

namespace starlix::gui {

static StarlixBootInfo* g_fb = nullptr;
static uint32_t cursor_bg_buffer[12][9];
static uint32_t prev_cursor_x = 720;
static uint32_t prev_cursor_y = 220;
static bool cursor_bg_saved = false;
static void gui_draw_usb_input_status(const char* text);
static uint32_t gui_mouse_x = 0u;
static uint32_t gui_mouse_y = 0u;

// Custom freestanding strlen
static size_t str_len(const char* s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static const uint8_t FONT_8X8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
};

void gui_init(StarlixBootInfo* bootInfo) {
    g_fb = bootInfo;
    cursor_bg_saved = false;
    gui_mouse_x = (g_fb && g_fb->FramebufferWidth) ? g_fb->FramebufferWidth / 2u : 0u;
    gui_mouse_y = (g_fb && g_fb->FramebufferHeight) ? g_fb->FramebufferHeight / 2u : 0u;
}

static uint32_t ConvertFramebufferColor(
    const StarlixBootInfo* info, uint32_t color) {
    if (info &&
        info->FramebufferPixelFormat ==
            StarlixFramebufferPixelFormatRgbReserved8) {
        return ((color & 0x000000FFu) << 16) |
               (color & 0x0000FF00u) |
               ((color & 0x00FF0000u) >> 16);
    }
    return color;
}

static void PutPixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_fb || !g_fb->FramebufferBase) return;
    if (x >= g_fb->FramebufferWidth || y >= g_fb->FramebufferHeight) return;
    if (g_fb->FramebufferPixelsPerScanLine < g_fb->FramebufferWidth) return;

    uint64_t index =
        static_cast<uint64_t>(y) * g_fb->FramebufferPixelsPerScanLine + x;
    uint64_t capacity = g_fb->FramebufferSize / sizeof(uint32_t);
    if (index >= capacity) return;

    uint32_t* fb = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(g_fb->FramebufferBase));
    fb[index] = ConvertFramebufferColor(g_fb, color);
}

static uint32_t GetPixel(uint32_t x, uint32_t y) {
    if (!g_fb || !g_fb->FramebufferBase) return 0;
    if (x >= g_fb->FramebufferWidth || y >= g_fb->FramebufferHeight) return 0;
    if (g_fb->FramebufferPixelsPerScanLine < g_fb->FramebufferWidth) return 0;

    uint64_t index =
        static_cast<uint64_t>(y) * g_fb->FramebufferPixelsPerScanLine + x;
    uint64_t capacity = g_fb->FramebufferSize / sizeof(uint32_t);
    if (index >= capacity) return 0;

    uint32_t* fb = reinterpret_cast<uint32_t*>(
        static_cast<uintptr_t>(g_fb->FramebufferBase));
    return ConvertFramebufferColor(g_fb, fb[index]);
}

static void FillRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            PutPixel(x + i, y + j, color);
        }
    }
}

static void DrawChar(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t scale) {
    if (c < 32 || c > 127) c = '?';
    const uint8_t* glyph = FONT_8X8[c - 32];
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                for (uint32_t sy = 0; sy < scale; sy++)
                    for (uint32_t sx = 0; sx < scale; sx++)
                        PutPixel(x + col * scale + sx, y + row * scale + sy, color);
            }
        }
    }
}

static void DrawString(uint32_t x, uint32_t y, const char* s, uint32_t color, uint32_t scale) {
    uint32_t cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 8 * scale + 4; cx = x; continue; }
        DrawChar(cx, y, *s, color, scale);
        cx += 8 * scale + 2;
    }
}

void DrawLockScreen(const char* pass_buffer) {
    if (!g_fb) return;
    uint32_t screen_w = g_fb->FramebufferWidth;
    uint32_t screen_h = g_fb->FramebufferHeight;

    FillRect(0, 0, screen_w, screen_h, 0x000F172A);

    DrawString((screen_w / 2) - 100, (screen_h / 2) - 180, "12:00", 0x00F8FAFC, 6);
    DrawString((screen_w / 2) - 140, (screen_h / 2) - 110, "Monday, October 12", 0x0038BDF8, 2);

    uint32_t avatar_x = (screen_w / 2) - 32;
    uint32_t avatar_y = (screen_h / 2) - 40;
    FillRect(avatar_x, avatar_y, 64, 64, 0x001E293B);
    DrawChar(avatar_x + 24, avatar_y + 16, 'U', 0x00F8FAFC, 4);

    DrawString((screen_w / 2) - 60, avatar_y + 75, "Starlix User", 0x00F8FAFC, 2);

    uint32_t box_w = 260, box_h = 36;
    uint32_t box_x = (screen_w / 2) - (box_w / 2);
    uint32_t box_y = avatar_y + 110;

    FillRect(box_x - 1, box_y - 1, box_w + 2, box_h + 2, 0x0038BDF8);
    FillRect(box_x, box_y, box_w, box_h, 0x001E293B);

    if (pass_buffer && pass_buffer[0] != '\0') {
        char mask[32] = {0};
        size_t len = str_len(pass_buffer);
        for (size_t i = 0; i < len && i < 30; i++) mask[i] = '*';
        DrawString(box_x + 12, box_y + 10, mask, 0x00F8FAFC, 2);
    } else {
        DrawString(box_x + 12, box_y + 10, "Press ENTER to Unlock", 0x0064748B, 1);
    }
}

void DrawDesktopShell() {
    if (!g_fb) return;
    uint32_t screen_w = g_fb->FramebufferWidth;
    uint32_t screen_h = g_fb->FramebufferHeight;

    uint32_t tb_h = 48;
    uint32_t tb_y = screen_h - tb_h;
    FillRect(0, tb_y, screen_w, tb_h, 0x001E293B);
    FillRect(0, tb_y, screen_w, 1, 0x00334155);

    uint32_t start_x = (screen_w / 2) - 16;
    uint32_t start_y = tb_y + 8;
    FillRect(start_x, start_y, 32, 32, 0x0038BDF8);
    DrawChar(start_x + 10, start_y + 8, 'S', 0x000F172A, 2);

    DrawString(screen_w - 90, tb_y + 16, "12:00 PM", 0x00F8FAFC, 1);
}

void DrawWindow(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const char* title) {
    FillRect(x + 6, y + 6, width, height, 0x00020617);
    FillRect(x - 1, y - 1, width + 2, height + 2, 0x00475569);
    FillRect(x, y, width, height, 0x00F8FAFC);
    FillRect(x, y, width, 32, 0x000F172A);
    DrawString(x + 12, y + 8, title, 0x00F8FAFC, 1);
    FillRect(x + width - 36, y + 4, 28, 24, 0x00EF4444);
    DrawChar(x + width - 26, y + 8, 'X', 0x00FFFFFF, 1);
}

void DrawWindowText(uint32_t win_x, uint32_t win_y, uint32_t rel_x, uint32_t rel_y, const char* text, uint32_t color) {
    DrawString(win_x + rel_x, win_y + 32 + rel_y, text, color, 1);
}

[[maybe_unused]] void DrawMouseCursor(uint32_t x, uint32_t y) {
    uint32_t cursor_map[12][9] = {
        {1,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,1,1,1,1},
        {1,2,1,1,2,1,0,0,0},
        {1,1,0,0,1,2,1,0,0},
        {1,0,0,0,0,1,1,0,0}
    };

    if (cursor_bg_saved) {
        for (int j = 0; j < 12; j++) {
            for (int i = 0; i < 9; i++) {
                PutPixel(prev_cursor_x + i, prev_cursor_y + j, cursor_bg_buffer[j][i]);
            }
        }
    }

    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 9; i++) {
            cursor_bg_buffer[j][i] = GetPixel(x + i, y + j);
        }
    }

    prev_cursor_x = x;
    prev_cursor_y = y;
    cursor_bg_saved = true;

    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 9; i++) {
            if (cursor_map[j][i] == 1) {
                PutPixel(x + i, y + j, 0x00000000);
            } else if (cursor_map[j][i] == 2) {
                PutPixel(x + i, y + j, 0x00FFFFFF);
            }
        }
    }
}

static void gui_draw_usb_input_status(const char* text) {
    // Keyboard diagnostics stay off the kernel framebuffer. The CPL3 Lock
    // Screen owns all visible input/status presentation.
    (void)text;
}

/* Minimal visible keyboard text consumer. It is not a shell or a full text editor. */
static constexpr uint32_t GUI_KEYBOARD_TEXT_CAPACITY = 128u;
static char gui_keyboard_text[GUI_KEYBOARD_TEXT_CAPACITY]{};
static char gui_keyboard_last_line[GUI_KEYBOARD_TEXT_CAPACITY]{};
static uint32_t gui_keyboard_text_length = 0u;
static uint32_t gui_keyboard_last_length = 0u;
static bool gui_keyboard_show_last_line = false;
static bool gui_keyboard_caps_lock = false;

static void gui_keyboard_draw_text() {
    const char* prefix = gui_keyboard_show_last_line &&
                         gui_keyboard_text_length == 0u ? "LAST: " : "TYPE: ";
    const char* content = gui_keyboard_show_last_line &&
                          gui_keyboard_text_length == 0u
        ? gui_keyboard_last_line : gui_keyboard_text;

    uint32_t prefix_length = 0u;
    while (prefix[prefix_length] != '\0') ++prefix_length;

    uint32_t content_length = gui_keyboard_show_last_line &&
                              gui_keyboard_text_length == 0u
        ? gui_keyboard_last_length : gui_keyboard_text_length;

    const uint32_t width = g_fb && g_fb->FramebufferWidth < 240u
        ? g_fb->FramebufferWidth : 240u;
    const uint32_t columns = width > 16u ? (width - 16u) / 8u : 1u;
    const uint32_t content_columns = columns > prefix_length
        ? columns - prefix_length : 0u;
    const uint32_t first = content_length > content_columns
        ? content_length - content_columns : 0u;

    char display[64]{};
    uint32_t pos = 0u;
    for (uint32_t i = 0u; i < prefix_length && pos + 1u < sizeof(display); ++i)
        display[pos++] = prefix[i];

    for (uint32_t i = first;
         i < content_length && pos + 1u < sizeof(display); ++i)
        display[pos++] = content[i];

    display[pos] = '\0';
    gui_draw_usb_input_status(display);
}

static void gui_keyboard_append(char ch) {
    if (gui_keyboard_text_length + 1u >= GUI_KEYBOARD_TEXT_CAPACITY) {
        gui_draw_usb_input_status("INPUT BUFFER FULL");
        return;
    }
    gui_keyboard_text[gui_keyboard_text_length++] = ch;
    gui_keyboard_text[gui_keyboard_text_length] = '\0';
    gui_keyboard_show_last_line = false;
}

void gui_handle_input(const InputEvent* event) {
    if (!event || !g_fb || !g_fb->FramebufferBase) return;

    if (event->type == InputEventType::KeyDown ||
        event->type == InputEventType::KeyUp) {
        if (event->type == InputEventType::KeyUp) {
            gui_keyboard_draw_text();
            return;
        }

        const uint8_t usage = event->code;

        if (usage == 0x39u) { // Caps Lock: toggle on the down transition only.
            gui_keyboard_caps_lock = !gui_keyboard_caps_lock;
            gui_keyboard_show_last_line = false;
            gui_keyboard_draw_text();
            return;
        }

        if (usage == 0x29u) { // Escape clears the diagnostic line.
            gui_keyboard_text_length = 0u;
            gui_keyboard_text[0] = '\0';
            gui_keyboard_show_last_line = false;
            gui_draw_usb_input_status("INPUT CLEARED");
            return;
        }

        if (usage == 0x2Au) { // Backspace
            if (gui_keyboard_text_length != 0u) {
                --gui_keyboard_text_length;
                gui_keyboard_text[gui_keyboard_text_length] = '\0';
                gui_keyboard_show_last_line = false;
            } else if (gui_keyboard_show_last_line) {
                gui_keyboard_last_length = 0u;
                gui_keyboard_last_line[0] = '\0';
                gui_keyboard_show_last_line = false;
            }
            gui_keyboard_draw_text();
            return;
        }

        if (usage == 0x28u) { // Enter: retain and show the last submitted line.
            if (gui_keyboard_text_length == 0u) {
                gui_draw_usb_input_status("ENTER (EMPTY)");
                return;
            }
            gui_keyboard_last_length = gui_keyboard_text_length;
            for (uint32_t i = 0u; i < gui_keyboard_text_length; ++i)
                gui_keyboard_last_line[i] = gui_keyboard_text[i];
            gui_keyboard_last_line[gui_keyboard_last_length] = '\0';
            gui_keyboard_text_length = 0u;
            gui_keyboard_text[0] = '\0';
            gui_keyboard_show_last_line = true;
            gui_keyboard_draw_text();
            return;
        }

        const bool control_alt_or_gui = (event->modifiers & 0xDDu) != 0u;
        if (control_alt_or_gui) {
            // Do not insert control combinations as printable characters.
            gui_keyboard_draw_text();
            return;
        }

        if (usage == 0x2Bu) { // Tab: visible four-space indentation.
            gui_keyboard_append(' ');
            gui_keyboard_append(' ');
            gui_keyboard_append(' ');
            gui_keyboard_append(' ');
            gui_keyboard_draw_text();
            return;
        }

        const bool shifted = (event->modifiers & 0x22u) != 0u;
        const char ch = starlix::gui::keyboard_text::usage_to_ascii(
            usage, shifted, gui_keyboard_caps_lock);
        if (ch != '\0') {
            gui_keyboard_append(ch);
            gui_keyboard_draw_text();
        }
        return;
    }

    if (event->type == InputEventType::MouseReport) {
        int32_t next_x = static_cast<int32_t>(gui_mouse_x) + event->dx;
        int32_t next_y = static_cast<int32_t>(gui_mouse_y) + event->dy;
        const int32_t max_x = g_fb->FramebufferWidth > 9u
            ? static_cast<int32_t>(g_fb->FramebufferWidth - 9u) : 0;
        const int32_t max_y = g_fb->FramebufferHeight > 12u
            ? static_cast<int32_t>(g_fb->FramebufferHeight - 12u) : 0;
        if (next_x < 0) next_x = 0;
        if (next_y < 0) next_y = 0;
        if (next_x > max_x) next_x = max_x;
        if (next_y > max_y) next_y = max_y;
        gui_mouse_x = static_cast<uint32_t>(next_x);
        gui_mouse_y = static_cast<uint32_t>(next_y);
        if (event->buttons != 0u) {
            char text[] = "USB HID MOUSE BUTTONS 0";
            text[22] = static_cast<char>('0' + (event->buttons & 0x07u));
            text[23] = '\0';
            gui_draw_usb_input_status(text);
        }
    }
}
} // namespace starlix::gui
