#pragma once

namespace starlix::gui::keyboard_text {

// USB HID Keyboard/Keypad usage IDs to a US-layout character.
// Shift and Caps Lock interact only for alphabetic keys.
inline char usage_to_ascii(unsigned char usage, bool shifted, bool caps_lock) {
    if (usage >= 0x04u && usage <= 0x1Du) {
        const bool upper = shifted != caps_lock;
        const char lower_char = static_cast<char>('a' + (usage - 0x04u));
        return upper ? static_cast<char>(lower_char - 'a' + 'A') : lower_char;
    }

    if (usage >= 0x1Eu && usage <= 0x27u) {
        static constexpr char digits[] = "1234567890";
        static constexpr char shifted_digits[] = "!@#$%^&*()";
        const unsigned int index = usage - 0x1Eu;
        return shifted ? shifted_digits[index] : digits[index];
    }

    switch (usage) {
        case 0x2Cu: return ' ';
        case 0x2Du: return shifted ? '_' : '-';
        case 0x2Eu: return shifted ? '+' : '=';
        case 0x2Fu: return shifted ? '{' : '[';
        case 0x30u: return shifted ? '}' : ']';
        case 0x31u: return shifted ? '|' : '\\';
        case 0x32u: return shifted ? '~' : '#';
        case 0x33u: return shifted ? ':' : ';';
        case 0x34u: return shifted ? '"' : '\'';
        case 0x35u: return shifted ? '~' : '`';
        case 0x36u: return shifted ? '<' : ',';
        case 0x37u: return shifted ? '>' : '.';
        case 0x38u: return shifted ? '?' : '/';
        default: return '\0';
    }
}

} // namespace starlix::gui::keyboard_text
