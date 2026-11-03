// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "efi/runtime.hpp"

extern "C" void* memcpy(void* dest, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long long i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

extern "C" void* memset(void* dest, int val, unsigned long long n) {
    unsigned char* d = (unsigned char*)dest;
    unsigned char v = (unsigned char)val;
    for (unsigned long long i = 0; i < n; i++) {
        d[i] = v;
    }
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, unsigned long long n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0) return dest;

    if (d < s) {
        for (unsigned long long i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (unsigned long long i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

extern "C" int memcmp(const void* s1, const void* s2, unsigned long long n) {
    const unsigned char* a = (const unsigned char*)s1;
    const unsigned char* b = (const unsigned char*)s2;
    for (unsigned long long i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}
