// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
// ============================================================
// Starlix OS - Freestanding Runtime Primitives
// Replacement for libc memcpy/memset/memmove/memcmp
// Required because -nodefaultlib removes all CRT/libc linkage,
// but the compiler still emits implicit calls to these symbols
// for struct copies, array initialization, etc.
// ============================================================

extern "C" void* memcpy(void* dest, const void* src, unsigned long long n);
extern "C" void* memset(void* dest, int val, unsigned long long n);
extern "C" void* memmove(void* dest, const void* src, unsigned long long n);
extern "C" int   memcmp(const void* s1, const void* s2, unsigned long long n);
