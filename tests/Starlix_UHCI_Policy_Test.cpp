#include "../kernel/src/drivers/uhci_policy.hpp"
int main() { return starlix::drivers::uhci::policy::self_test() ? 0 : 1; }
