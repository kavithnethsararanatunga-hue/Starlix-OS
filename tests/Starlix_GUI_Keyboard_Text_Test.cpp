#include "../kernel/src/gui/keyboard_text.hpp"
#include <cstdio>

using starlix::gui::keyboard_text::usage_to_ascii;

static int failures = 0;

static void check(const char* name, unsigned char usage,
                  bool shift, bool caps, char expected) {
    const char actual = usage_to_ascii(usage, shift, caps);
    if (actual == expected) {
        std::printf("PASS: %s\n", name);
    } else {
        std::printf("FAIL: %s (expected 0x%02X, got 0x%02X)\n",
                    name,
                    static_cast<unsigned int>(static_cast<unsigned char>(expected)),
                    static_cast<unsigned int>(static_cast<unsigned char>(actual)));
        ++failures;
    }
}

int main() {
    check("A usage -> lowercase a", 0x04u, false, false, 'a');
    check("Shift+A -> uppercase A", 0x04u, true, false, 'A');
    check("Caps Lock+A -> uppercase A", 0x04u, false, true, 'A');
    check("Caps Lock+Shift+A -> lowercase a", 0x04u, true, true, 'a');
    check("Z usage -> lowercase z", 0x1Du, false, false, 'z');
    check("number 1", 0x1Eu, false, false, '1');
    check("Shift+1 -> !", 0x1Eu, true, false, '!');
    check("number 0", 0x27u, false, false, '0');
    check("Shift+0 -> )", 0x27u, true, false, ')');
    check("space", 0x2Cu, false, false, ' ');
    check("semicolon", 0x33u, false, false, ';');
    check("Shift+semicolon -> colon", 0x33u, true, false, ':');
    check("slash", 0x38u, false, false, '/');
    check("Shift+slash -> question mark", 0x38u, true, false, '?');
    check("modifier usage is not printable", 0xE1u, false, false, '\0');

    std::printf("Keyboard character-map test: %s (%d failures)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
