#include "../kernel/src/drivers/framebuffer_policy.hpp"
using namespace starlix::drivers::framebuffer::policy;
int main() {
    constexpr unsigned int RGB = 1u, BGR = 2u;
    if (!valid_geometry(0xE0000000ULL, 4ULL * 1024ULL * 1024ULL,
                        1024u, 768u, 1024u, BGR, RGB, BGR)) return 1;
    if (valid_geometry(0ULL, 4ULL * 1024ULL * 1024ULL,
                       1024u, 768u, 1024u, BGR, RGB, BGR)) return 2;
    if (valid_geometry(0xFFFFFFF0ULL, 0x100ULL,
                       320u, 240u, 320u, BGR, RGB, BGR)) return 3;
    if (valid_geometry(0xE0000000ULL, 1024ULL,
                       1024u, 768u, 1024u, BGR, RGB, BGR)) return 4;
    if (valid_geometry(0xE0000000ULL, 4ULL * 1024ULL * 1024ULL,
                       4097u, 768u, 4097u, BGR, RGB, BGR)) return 5;
    if (!valid_blit(0u, 0u, 64u, 64u, 64u, 1024u, 768u)) return 6;
    if (!valid_blit(960u, 704u, 64u, 64u, 64u, 1024u, 768u)) return 7;
    if (valid_blit(961u, 704u, 64u, 64u, 64u, 1024u, 768u)) return 8;
    if (valid_blit(0u, 705u, 64u, 64u, 64u, 1024u, 768u)) return 9;
    if (valid_blit(0u, 0u, 65u, 64u, 64u, 1024u, 768u)) return 10;
    if (valid_blit(0u, 0u, 64u, 64u, 63u, 1024u, 768u)) return 11;
    if (fade_remaining(0u, 20u) != 255u) return 12;
    if (fade_remaining(10u, 20u) != 127u) return 13;
    if (fade_remaining(20u, 20u) != 0u) return 14;
    if (fade_remaining(21u, 20u) != 0u || fade_remaining(0u, 0u) != 0u) return 15;
    if (fade_scale_channel(200u, 127u, 255u) != 100u) return 16;
    if (fade_scale_channel(200u, 0u, 255u) != 0u) return 17;
    if (fade_scale_channel(200u, 255u, 255u) != 200u) return 18;
    return 0;
}
