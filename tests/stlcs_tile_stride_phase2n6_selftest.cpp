#include "../user_components/stlcs/stlcs_abi.hpp"
#include <array>
#include <cstdint>
#include <cstdio>

int main() {
    using namespace starlix::stlcs;
    std::array<bool, DISPLAY_TILE_MAX_PIXELS> seen{};
    for (stlcs_u32 row = 0; row < DISPLAY_TILE_EDGE; ++row) {
        for (stlcs_u32 column = 0; column < DISPLAY_TILE_EDGE; ++column) {
            const stlcs_u32 index = display_tile_pixel_index(row, column);
            if (index >= DISPLAY_TILE_MAX_PIXELS || seen[index]) {
                std::fprintf(stderr, "FAIL: invalid/duplicate tile index row=%u col=%u index=%u\n",
                    row, column, index);
                return 1;
            }
            seen[index] = true;
        }
    }
    for (bool value : seen) {
        if (!value) {
            std::fprintf(stderr, "FAIL: tile index map contains a gap\n");
            return 1;
        }
    }
    if (display_tile_pixel_index(31u, 31u) != 1023u ||
        display_tile_pixel_index(21u, 18u) != 690u) {
        std::fprintf(stderr, "FAIL: tile row stride is not the shared 32-pixel ABI stride\n");
        return 1;
    }
    /* A clipped edge tile still uses the fixed 32-pixel source stride. */
    if (display_tile_pixel_index(0u, 6u) != 6u ||
        display_tile_pixel_index(1u, 0u) != 32u ||
        display_tile_pixel_index(2u, 6u) != 70u) {
        std::fprintf(stderr, "FAIL: clipped edge tile changed its packed source stride\n");
        return 1;
    }
    std::puts("STLCS_TILE_STRIDE_PHASE2N6: PASS (1024 unique bounded indices)");
    return 0;
}
