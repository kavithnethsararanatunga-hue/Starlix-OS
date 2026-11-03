// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../cpu/io.hpp"

namespace starlix::drivers::serial {
    inline void init() {
        starlix::cpu::outb(0x3F8 + 1, 0x00);
        starlix::cpu::outb(0x3F8 + 3, 0x80);
        starlix::cpu::outb(0x3F8 + 0, 0x03);
        starlix::cpu::outb(0x3F8 + 1, 0x00);
        starlix::cpu::outb(0x3F8 + 3, 0x03);
        starlix::cpu::outb(0x3F8 + 2, 0xC7);
        starlix::cpu::outb(0x3F8 + 4, 0x0B);
    }

    inline void print(const char* str) {
        while (*str) {
            while ((starlix::cpu::inb(0x3F8 + 5) & 0x20) == 0);
            starlix::cpu::outb(0x3F8, *str++);
        }
    }
}
