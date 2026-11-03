#include "../kernel/src/drivers/ehci_hub_policy.hpp"

int main() {
    return starlix::drivers::ehci::hub_policy::self_test() ? 0 : 1;
}
