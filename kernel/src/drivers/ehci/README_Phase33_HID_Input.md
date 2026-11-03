# Phase 33 — USB HID Boot Input Integration

## What this change implements

- Discovers HID **boot keyboard** (`class=0x03, subclass=1, protocol=1`) and
  **boot mouse** (`class=0x03, subclass=1, protocol=2`) interfaces inside the
  already-validated configuration descriptor.
- Selects USB HID Boot Protocol with `SET_PROTOCOL`; sends `SET_IDLE` as a
  best-effort request.
- Opens usable interrupt-IN endpoints through the existing persistent EHCI
  periodic endpoint manager, bounded by its fixed eight-endpoint capacity.
- Harvests reports from normal kernel context, decodes 8-byte keyboard reports
  and 3/4-byte mouse reports, and publishes key-down/key-up/mouse-report events
  to a fixed 128-event queue.
- Emits key transitions (not repeated key-down events for every poll), ignores
  keyboard rollover reports without synthesizing key releases, and records queue
  overflow via `ehci_hid_dropped_event_count()`.
- Connects the queue to the current GUI framebuffer input path: mouse reports
  move the drawn cursor; keyboard reports display the most recent HID usage
  code/status. The public queue API is also available for a future text/input
  widget.
- Adds pure policy tests for HID-interface selection, report transitions,
  mouse deltas, queue bounds, USB configuration parsing, and driver binding.

## Current support limits — important

The existing EHCI controller implementation enumerates **high-speed direct
root-port devices only**. The persistent periodic QH manager programs
`QH_SPEED_HIGH`, supports one transaction per endpoint, and accepts `bInterval`
1–14. The new binder therefore fails closed on endpoint packet formats or
intervals outside those constraints. This phase does not add split-transaction
support for low/full-speed devices behind a high-speed hub.

When a connected root-port device is low/full speed, current EHCI code hands the
port to a companion controller. There is no OHCI/UHCI companion host-controller
driver in this change. The keyboard and mouse shown in the user's earlier host
output were reported as USB 1.1 Low speed, so this phase alone does **not** prove
that those devices will work in Starlix. A companion HCD (most likely OHCI for
the configured VirtualBox controller) plus shared HID-class binding is still
needed for them; this is USB host-controller support, not a PS/2 driver.

Raw HID usages are not yet translated through keyboard layouts into Unicode,
and the current GUI has no text-entry focus/window-manager click-routing layer.
This phase proves a code path from an eligible EHCI HID boot interface to
kernel input events and a minimal framebuffer indicator; it is not a complete
Windows-equivalent input stack.

## Validation

Run from the Starlix source root:

```bash
bash scripts/test_ehci_hid_input.sh
bash scripts/build_kernel.sh
bash scripts/build_boot_kernel.sh
```

The host policy test and freestanding kernel build passed during package
creation. The full boot-image build could not finish in the package build
container because `mkfs.vfat` was not installed, and no physical USB device or
VirtualBox runtime was available for an end-to-end transfer test there. Run the
full image build and VM validation on the user's Ubuntu host before treating
hardware input as verified.
