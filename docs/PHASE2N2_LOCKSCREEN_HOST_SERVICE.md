# Starlix OS 1.0 Beta — Phase 2N.2 Lock Screen Host Services

## Scope

Phase 2N.2 connects the production `RXFS/sys/User/stlcs.ect` component to a narrowly scoped CPL3 host-service transport and a bounded GOP framebuffer copy service. The Lock Screen renderer and font/layout data execute in `stlcs.ect` user mode. The kernel service copies validated pixel tiles; it does not implement Lock Screen text layout, windowing, or authentication UI.

This phase builds on Phase 2N.1 ATA PIO + RXFS support. It does not claim a working desktop unlock path.

## Startup path

1. Kernel storage initialization discovers the VMware PIIX4 IDE device and mounts RXFS read-only.
2. `system_component.cpp` resolves the exact `RXFS/sys/User/stlcs.ect` path, reads the complete file into a bounded kernel buffer, and validates it with the production ECT v1 loader.
3. The kernel verifies the DPL3 INT `0x81` gate and validated GOP framebuffer transport are ready.
4. The ECT loader maps the component into an independent user address space and creates its process/thread with the existing Phase 2L/2M machinery.
5. The Lock Screen thread is registered with the restricted HostApi dispatcher and entered through the existing first-user IRETQ path.
6. `stlcs_bootstrap` establishes SysV stack alignment, creates the HostApi and Session on its user stack, then runs the actual `stlcs_entry` code.
7. The component draws its own Lock Screen in CPL3 and sends 64x64 pixel tiles through the bounded kernel framebuffer copy service.

## Restricted service ABI

The service transport is a fixed DPL3 software interrupt gate, `INT 0x81`. It is not a general syscall ABI. It does not enable `EFER.SCE`, `SYSCALL`, or `SYSRET`.

Register contract:

- `RAX`: fixed service identifier.
- `RDI`: service ABI version, currently `1`.
- `RSI`, `RDX`, `RCX`, `R8`, `R9`: fixed operation arguments.
- `RAX` on return: operation result (`1` success, `0` unavailable/denied/no event where specified, `~0ULL` invalid request).

Only the registered `stlcs` main thread may call the gate. The dispatcher verifies the vector, CPL3 return frame, user RIP/RSP mappings, process/thread registry and states, active CR3, TSS.RSP0, guarded kernel stack, image identity, and ABI version before dispatching a service.

| ID | Service | Behavior |
|---:|---|---|
| 1 | PollInput | Copies one validated key event from the EHCI HID queue into a user buffer. Empty queue returns 0. |
| 4 | Authenticate | Validates bounded user strings, copies them into kernel-owned buffers, wipes both buffers, then returns `ServiceUnavailable` until a trusted authentication endpoint exists. |
| 5 | RequestDesktop | Validates the user name and always denies handoff until a trusted auth token and session manager exist. |
| 6 | Yield | Uses a bounded `STI; HLT; CLI` idle point so the PIT can preempt the user thread. |
| 7 | DisplayInfo | Copies a validated framebuffer description to a user buffer. |
| 8 | DisplayBlit | Validates the request, user buffer range, tile dimensions, source stride, and transfer size; copies at most 4096 pixels into kernel-owned memory before the framebuffer write. |
| 9 | FrameComplete | Marks component-active only after a complete user-mode frame has been submitted through the tile transport. |

Unknown IDs, wrong ABI versions, unregistered callers, invalid frames, unmapped buffers, invalid permissions, and oversized transfers fail closed.

## Display architecture

`user_components/stlcs/stlcs.cpp` contains the Lock Screen drawing logic and `font8x8.hpp` contains its font data. The kernel `drivers/framebuffer` module is a transport endpoint only: it validates UEFI GOP geometry and copies bounded pixel tiles. It does not render text or determine the Lock Screen layout.

Framebuffer transport accepts only RGBReserved8 or BGRReserved8 GOP modes, dimensions from 320x240 through 4096x2160, and a framebuffer range wholly below 4 GiB (the current kernel's identity-mapped range). Each blit is limited to 64x64 pixels.

## Deliberate limitations / fail-closed behavior

- The current VMware VMX previously reported no EHCI or UHCI controller. In that hardware configuration the user-mode screen can render, but the keyboard input queue remains empty. Connect a supported USB controller/device before expecting interactive key input.
- There is no trusted `lsass.ect` authentication endpoint in this milestone. Authentication always returns `ServiceUnavailable`; password data is wiped and never logged.
- There is no trusted desktop session manager or authenticated session token. Desktop handoff is always denied.
- A production component fault/termination/recovery lifecycle is not completed by this patch. An exception can still escalate through the kernel's existing exception path. Do not claim fault-recovery acceptance until separately implemented and tested.
- A successful build, ECT parse, process creation, or prepared IRET frame is not proof of hardware CPL3 execution. VMware serial must show `[RING3] STLCS_ENTRY=EXECUTING` and `[LOCKSCREEN] COMPONENT_ACTIVE=PASS`, which are emitted only after the user component has submitted a full frame.

## Build and tests

Run `bash tools/build_starlix_phase2n2_candidate.sh <phase10-run-directory>` from WSL Ubuntu. It builds `stlcs.ect`, runs the Phase 2N.2 policy/contract, framebuffer, ECT parser, RXFS fixture and native RXFS tests, builds normal and opt-in diagnostic kernels, restores the normal production kernel as the default build, and creates a new disposable Phase 12 VMware candidate.

Do not overwrite the existing Phase 2M/2N.1 candidates or the Phase 10 original image.
