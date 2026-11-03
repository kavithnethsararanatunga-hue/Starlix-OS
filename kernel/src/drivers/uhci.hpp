/* USB 1.1 UHCI companion-controller interface. */
#pragma once
#include "ehci.hpp"

namespace starlix::drivers::uhci {
bool uhci_init();
bool uhci_is_active();
void uhci_begin_enumeration(bool low_speed);
void uhci_end_enumeration();
bool uhci_owns_device(uint8_t address);
void uhci_set_device(uint8_t address, bool low_speed, uint8_t ep0_packet);
void uhci_set_port_device_address(uint8_t port, uint8_t address);
bool uhci_control_transfer(uint8_t address, starlix::drivers::ehci::USB_SETUP_PACKET* setup,
                           void* data, uint16_t length);
bool uhci_periodic_interrupt_open(uint8_t address, uint8_t endpoint, uint16_t packet,
                                  uint8_t interval, uint32_t* id);
starlix::drivers::ehci::EHCIPeriodicPollResult uhci_periodic_interrupt_poll(
    uint32_t id, void* data, uint16_t capacity, uint16_t* actual);
bool uhci_periodic_interrupt_close(uint32_t id);
void uhci_scan_ports();
bool uhci_service_port_changes();
bool uhci_port_change_pending();
bool uhci_forget_device(uint8_t address);
uint32_t uhci_periodic_interrupt_active_count();
}
