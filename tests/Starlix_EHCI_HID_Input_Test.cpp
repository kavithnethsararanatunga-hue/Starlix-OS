#include <cstddef>
#include <cstdint>
#include <cstdio>
#define STARLIX_PRIMITIVE_TYPES_DEFINED
#include "drivers/ehci_hid_input_policy.hpp"
#include "drivers/ehci_usb_core_policy.hpp"
#include "drivers/ehci_device_model_policy.hpp"
#include "drivers/ehci_class_registry_policy.hpp"

int main() {
    using namespace starlix::drivers::ehci;
    if (!hid_policy::self_test()) {
        std::puts("FAIL: descriptor and boot-report policy");
        return 1;
    }
    if (!hid_input::self_test()) {
        std::puts("FAIL: HID input event queue/report delta");
        return 2;
    }
    if (!usb_core::configuration_self_test()) {
        std::puts("FAIL: configuration parser HID interface coverage");
        return 3;
    }
    if (!device_model_policy::self_test()) {
        std::puts("FAIL: USB device model HID binding policy");
        return 4;
    }
    if (!class_registry::self_test()) {
        std::puts("FAIL: USB class registry");
        return 5;
    }
    std::puts("PASS: EHCI HID input policy, event transitions, mouse reports, queue bounds, configuration parser and class binding");
    return 0;
}
