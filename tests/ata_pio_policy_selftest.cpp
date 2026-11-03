#include "../kernel/src/drivers/ata_pio_policy.hpp"
#include <cstdio>

int main() {
    if (!starlix::drivers::ata::policy::self_test()) {
        std::fprintf(stderr, "ATA PIO policy self-test failed\n");
        return 1;
    }
    std::puts("ATA PIO policy self-test passed");
    return 0;
}
