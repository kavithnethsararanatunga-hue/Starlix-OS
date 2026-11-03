// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>

extern "C" {
    uint64_t StarlixDisplayString(const char* msg);
}

int main() {
    StarlixDisplayString("[OK] Ring 3 User App (.ect) Executing via libstarlix.lbr!");
    return 0;
}
