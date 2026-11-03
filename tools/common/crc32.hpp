// Starlix OS - OpenRPX xsOft - Trade secret. See project LICENSE.
#pragma once
#include <cstdint>
#include <cstddef>

namespace starlix {

inline uint32_t crc32_update(const uint8_t* data, size_t length, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

inline uint32_t compute_crc32(const uint8_t* data, size_t length) {
    return crc32_update(data, length) ^ 0xFFFFFFFFu;
}

} // namespace starlix
