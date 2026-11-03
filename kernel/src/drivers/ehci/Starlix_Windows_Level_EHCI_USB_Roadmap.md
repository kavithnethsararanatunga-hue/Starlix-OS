# Starlix OS — Windows-like EHCI / USB 2.0 Roadmap

**COPYRIGHT © 2026 OPENRPX XSOFT. ALL RIGHTS RESERVED.**

## Scope and expectations

This roadmap defines **32 phases** for building a robust, Windows-like **USB 2.0 host stack based on EHCI**, including core USB services and common device-class drivers. The number is a project-planning estimate, not a guarantee that every Windows feature or every device ever made will be supported. Windows has a layered USB architecture: host-controller support alone is not the whole USB stack.

EHCI is a USB 2.0 high-speed host-controller interface. It is not an xHCI/USB 3.x driver, and EHCI does not directly serve low-/full-speed devices in the same way as high-speed devices. Supporting those devices requires suitable USB 2.0 hub routing and/or UHCI/OHCI companion-controller support. USB 3.x SuperSpeed requires a separate xHCI implementation.

## Current Starlix baseline — Phases 1–5

Based on the user's supplied build output and QEMU serial logs, these milestones have been demonstrated in the current project. This does **not** mean the driver is production-ready or fully EHCI-specification compliant.

1. **EHCI discovery and bring-up:** PCI enumeration finds an EHCI-class controller; the log reports controller initialization and a high-speed root port.
2. **USB mass-storage enumeration:** USB address assignment, MSC Bulk-Only Transport interface binding, SCSI INQUIRY and READ CAPACITY(10) are reported.
3. **Initial storage path:** a USB block-device record is created and a read-only FAT32 volume can be mounted in the boot path.
4. **VFAT names and directories:** the QEMU test reads `HELLO.TXT`, `Long File Name.txt`, and `Docs/Nested Note.txt`.
5. **MBR-partitioned FAT32 runtime test:** QEMU reads the FAT32 partition starting at LBA 2048, including long-name and nested-file smoke tests.

**Validation limits:** the supplied logs establish QEMU-only runtime behavior. MBR partition mounting was runtime-tested; the supplied output shows GPT/CRC testing on the host-side test path, not a GPT-mounted QEMU runtime test. Real PC hardware, interrupt-driven operation, hot-plug, suspend/resume, robust recovery, and broad device-class coverage are still unverified.

## Phase plan

### Core controller and USB system

**Phase 6 — DMA and memory safety**
- Integrate EHCI with Starlix physical-memory and virtual-memory APIs.
- Define DMA-safe allocation, physical-address translation, alignment, cache-coherency rules, memory barriers, and lifetime rules for QHs, qTDs, frame lists, and buffers.
- Reject addresses that the controller cannot represent; never pass a virtual address as a DMA address without a valid mapping.
- **Exit criteria:** alignment and address-range unit tests pass; descriptor buffers remain valid until hardware ownership is conclusively released.

**Phase 7 — Controller lifecycle and register correctness**
- Harden BIOS/OS ownership handoff, controller reset/start/stop, capability/operational register parsing, PCI command bits, BAR validation, and USB legacy support handling.
- Correctly handle write-one-to-clear status bits and check run/stop and halted status with bounded timeouts.
- **Exit criteria:** repeated initialize/stop/reinitialize tests succeed in QEMU; unsupported BARs and timeout paths fail safely.

**Phase 8 — Interrupt framework and EHCI event handling**
- Add EHCI interrupt status/mask handling, interrupt routing to the kernel IDT/IRQ subsystem, deferred completion work, and safe acknowledgement of interrupt causes.
- Avoid doing lengthy descriptor walking or class-driver work in the hard interrupt path.
- **Exit criteria:** transfer completions and controller errors are reported without relying on busy-poll loops; spurious interrupts are handled safely.

**Phase 9 — General transfer engine**
- Implement reusable control and bulk transfer submission/completion with QH/qTD chains, multi-qTD requests, multi-page buffers, short packets, toggles, error status, cancellation, and transfer-size limits.
- Add clear request/endpoint ownership so a timed-out transfer cannot free memory still accessible to DMA.
- **Exit criteria:** descriptor-chain tests, short-packet tests, STALL/error tests, and timeout tests pass.

**Phase 10 — Asynchronous/periodic schedule management**
- Manage asynchronous schedule insertion/removal, safe QH reclamation, async-advance handling, periodic frame-list construction, and interrupt-endpoint polling intervals.
- Track bandwidth so endpoints do not exceed the available periodic budget.
- **Exit criteria:** multiple control/bulk endpoints and interrupt endpoints coexist without schedule corruption or unbounded resource growth.

**Phase 11 — Isochronous transfers**
- Implement EHCI iTD and siTD scheduling/completion, microframe accounting, frame-list constraints, and missed-service/error reporting.
- Treat real-time audio/video timing separately from bulk/control queue behavior.
- **Exit criteria:** test isochronous streams against a virtual or test device with measurable dropped/missed packet statistics.

**Phase 12 — Robust USB core and enumeration**
- Implement bounded, validated descriptor parsing for device, configuration, interface, endpoint, string, and class-specific descriptors.
- Handle address-zero enumeration, `GET_DESCRIPTOR`, `SET_ADDRESS`, `SET_CONFIGURATION`, alternate settings, endpoint halt clearing, and retries according to device state.
- Reject malformed descriptor lengths and inconsistent endpoint/configuration counts.
- **Exit criteria:** descriptor-fuzz tests do not crash the kernel; several different descriptor layouts enumerate correctly.

**Phase 13 — Root hub and port state machines**
- Model root-hub ports, connection/enable/suspend/reset/change bits, debounce, power control where supported, reset timing, and port-change notifications.
- Expose the root hub through the same hub/device model used by external hubs.
- **Exit criteria:** repeated attach/reset/enable cycles produce correct state transitions and bounded logs.

**Phase 14 — External hubs and speed routing**
- Add USB hub-class descriptors and requests, downstream port state machines, multi-level hub topology, and transaction-translator handling where required.
- Implement and test UHCI/OHCI companion handoff or documented supported-hub routing for low-/full-speed devices; EHCI by itself is not a universal low-/full-speed controller.
- **Exit criteria:** devices behind a hub enumerate at supported speeds; unsupported speed/routing combinations fail clearly instead of hanging.

**Phase 15 — Hot-plug and device lifetime**
- Add connect/disconnect detection, device-tree removal, endpoint teardown, request cancellation, address reuse, and safe cleanup when a device disappears mid-transfer.
- Protect against stale device pointers and DMA after disconnect.
- **Exit criteria:** repeated attach/detach during idle and during transfers does not leak resources, panic, or access freed memory.

**Phase 16 — Error recovery and concurrency**
- Add kernel locks/atomics appropriate to Starlix, request queues, timer-based timeouts, cancellation races, controller-halted/host-system-error recovery, endpoint reset, and controller reset fallback.
- Provide structured error codes and diagnostic counters rather than success-looking log text when operations fail.
- **Exit criteria:** injected STALL, timeout, disconnect, and controller-error tests recover or fail safely.

**Phase 17 — Power management**
- Implement USB device suspend/resume, port suspend/resume, remote-wakeup policy, idle schedule handling, and system sleep/resume integration where the kernel supports it.
- Respect each device's declared capabilities and avoid resuming a disconnected device.
- **Exit criteria:** suspend/resume cycles work repeatedly with devices attached; failures are bounded and diagnosable.

**Phase 18 — Device model, PnP, and driver binding**
- Create persistent device/interface/endpoint objects, stable IDs, compatible/class/subclass/protocol matching, attach/detach notifications, driver ownership, and resource teardown.
- Add composite-device support, multiple interfaces, alternate settings, interface association descriptors, and a generic parent model.
- **Exit criteria:** one composite device can bind separate interface drivers without one interface corrupting another.

### Storage and common device classes

**Phase 19 — Production-oriented USB Mass Storage BOT**
- Harden CBW/CSW validation, tags, residue/status handling, class reset recovery, endpoint halt clearing, retries, SCSI sense data, media-change detection, removable media, and multiple LUNs.
- Add write/flush commands only after DMA, cancellation, and error-recovery work is validated; default to read-only during early testing.
- **Exit criteria:** repeated read tests, injected command failures, no-media cases, and multiple-LUN tests pass on disposable images.

**Phase 20 — Storage integration and write support**
- Complete MBR and GPT runtime scanning (including GPT CRC and backup-header validation), bounds checks, block caching, block I/O completion/error propagation, and VFS mount/unmount integration.
- Add FAT32 create/write/rename/delete support only with consistency checks and explicit flush ordering. Add exFAT/NTFS only as separate filesystem projects with their own scope and licensing/specification review.
- **Exit criteria:** GPT is mounted in QEMU; file create/read/modify/reboot/read tests pass on disposable disk images; failures do not corrupt unrelated partitions.

**Phase 21 — HID input**
- Implement HID descriptor parsing, report-descriptor parsing, boot-protocol keyboard/mouse support, key rollover, modifiers, wheel/multiaxis mouse reports, and a generic HID input API.
- Add gamepad/joystick usage handling after basic HID stability.
- **Exit criteria:** keyboard and mouse work through direct and hub-connected ports; malformed reports are rejected.

**Phase 22 — CDC serial and modem control**
- Support CDC ACM control/data interfaces, line coding, control-line state, bulk/interrupt endpoint handling, serial buffering, and disconnect notification.
- **Exit criteria:** a supported USB serial adapter exposes a Starlix serial device with stable input/output and error handling.

**Phase 23 — USB networking**
- Add a bounded class-driver framework for supported CDC ECM/NCM and/or RNDIS devices, connect it to Starlix's network interface layer, and implement link-state/packet-buffer ownership rules.
- **Exit criteria:** DHCP and bidirectional network traffic work on supported QEMU/real devices; disconnects do not leave stale network interfaces.

**Phase 24 — USB audio**
- Implement relevant USB Audio Class versions, endpoint and alternate-setting selection, isochronous scheduling integration, feedback handling where required, sample formats/rates, and audio subsystem integration.
- **Exit criteria:** supported playback/capture modes run for sustained periods with reported underrun/overrun statistics.

**Phase 25 — USB video and imaging**
- Implement USB Video Class (UVC) descriptors and streaming control, isochronous/bulk video transport where defined, format negotiation, buffer queues, and a camera API. Scanner/image-class protocols may need separate support.
- **Exit criteria:** a declared supported camera profile streams frames without buffer overruns; unsupported profiles are rejected clearly.

**Phase 26 — USB printer support**
- Support the USB Printer Class protocol/interface where applicable, queueing, status/error handling, and integration with a print subsystem.
- **Exit criteria:** a compatible class-compliant printer completes a test job and reports out-of-paper/disconnect cases.

**Phase 27 — Vendor-specific devices and user-mode USB API**
- Add a permission-checked WinUSB-like generic interface for approved vendor-specific endpoints, request/pipe APIs, cancellation, and user/kernel buffer validation.
- This is an analogous Starlix API, not binary compatibility with Microsoft's `WinUSB.sys`.
- **Exit criteria:** a test user application can claim an allowed interface and exchange control/bulk data without kernel memory exposure.

**Phase 28 — Additional USB device classes**
- Add class drivers by priority: smart-card/CCID, MIDI, still-image/PTP, Bluetooth adapters, supported modem classes, and other required devices. Each class needs its own protocol conformance tests.
- Support vendor-specific devices only through explicit device-specific drivers or the generic USB API; no generic EHCI driver can infer every proprietary protocol.
- **Exit criteria:** every class advertised as supported has a documented device matrix and pass/fail tests.

### Security, conformance, and release quality

**Phase 29 — Security hardening and fuzzing**
- Fuzz USB descriptors, hub descriptors, transfer status, BOT CSWs, SCSI sense responses, partition tables, and FAT directory/LFN entries.
- Enforce strict bounds, overflow checks, privilege boundaries, and untrusted DMA-buffer validation.
- **Exit criteria:** fuzz corpus and fault-injection tests show no reproducible crash, out-of-bounds access, or use-after-free.

**Phase 30 — Automated regression and conformance suite**
- Build deterministic QEMU scenarios for enumeration, control/bulk/interrupt/isochronous transfers, disconnects, timeout/error handling, hubs, storage, HID, suspend/resume, MBR/GPT, and filesystem operations.
- Preserve serial logs and machine-readable pass/fail summaries for each build.
- **Exit criteria:** all must-pass tests run from one command and regressions fail CI/build gates.

**Phase 31 — Real hardware compatibility and performance**
- Test different EHCI implementations/chipsets, USB 2.0 hubs, flash drives, keyboards, mice, serial adapters, audio devices, and cameras; compare throughput, latency, CPU use, and error recovery.
- Verify real PCI BARs, interrupt routing, DMA address limits, firmware ownership, and behavior with memory above 4 GiB as applicable.
- **Exit criteria:** a published hardware matrix lists tested controller/device models, firmware settings, known limitations, and measured results. QEMU success alone is not enough.

**Phase 32 — Stable release, diagnostics, and maintenance**
- Add configuration switches, safe defaults, human-readable diagnostics, per-device capability reporting, debug counters, documentation, recovery procedures, and versioned driver interfaces.
- Keep unsupported features explicit; maintain regression tests for every fixed bug.
- **Exit criteria:** clean build, reproducible QEMU test run, documented physical-hardware validation, known-issues list, and safe rollback path.

## What “Windows-level” means for this roadmap

Use three acceptance tiers instead of claiming absolute parity:

- **Tier A — EHCI controller core:** robust DMA, schedules, interrupts, transfers, error recovery, and lifecycle.
- **Tier B — Windows-like USB 2.0 stack:** enumeration, hubs, hot-plug, PnP/device binding, power management, composite devices, storage/HID/serial and other common classes.
- **Tier C — broad ecosystem coverage:** more class drivers, vendor-specific APIs, extensive real-hardware matrix, security and long-duration reliability testing.

Completing all 32 phases is a target for a broad, reliable USB 2.0/EHCI stack. It does not make Starlix binary-compatible with Windows, does not provide xHCI/SuperSpeed, and cannot guarantee every proprietary USB device will work. Any further device class or vendor protocol can require additional driver-specific phases.

## Recommended next work

Start at **Phase 6: DMA and memory safety**. Before adding more device classes, inspect the current `phys.cpp`, `phys.hpp`, `vmm.cpp`, `vmm.hpp`, `ehci.cpp`, `ehci.hpp`, and the interrupt/timer APIs. Make the DMA mapping and descriptor lifetime rules explicit, add bounded timeout/error tests, and keep all experiments in QEMU or disposable disk images until validated.

## Phase 33 — HID boot-input integration (software implementation; hardware pending)

- Added actual EHCI configuration-descriptor discovery for HID boot keyboard and
  boot mouse interfaces, Boot Protocol selection, and persistent interrupt-IN
  endpoint binding through the existing periodic schedule manager.
- Added bounded key-down/key-up/mouse-report events, rollover-safe keyboard
  report delta processing, event queue overflow accounting, and an input path to
  the current framebuffer GUI (pointer cursor and last HID usage indicator).
- Added a host test for HID interface selection, key transitions, mouse reports,
  queue bounds, USB configuration parsing, and device-model binding; added a
  freestanding kernel build integration.
- Scope remains EHCI high-speed only: `QH_SPEED_HIGH`, one transaction, and
  `bInterval` 1–14. Low/full-speed root-port devices are handed to the companion
  controller by current code; no OHCI/UHCI HCD or EHCI split-transaction support
  is implemented here. The previously observed keyboard/mouse were USB 1.1 Low
  speed, so their input remains unverified and requires companion-HCD support.
- Host policy tests and kernel compile passed. The package container lacks
  `mkfs.vfat`, so final FAT32 ESP/disk-image build did not finish here. Physical
  transactions, real IRQ-driven input, key-layout translation, and VM/hardware
  tests remain open.

**Phase 33 state:** source implementation + policy tests + kernel compilation
verified; full boot image, guest input, hardware transfer, and release validation
remain open. This does not change the Beta release classification.
