# Starlix OS 1.0 Beta — Phase 2I: `stlcs.ect` Lock Screen Component

## Component identity

`stlcs.ect` is a native Starlix **user-mode system component**, not an `.app`
bundle. Its intended RXFS location is:

```text
RXFS/sys/User/stlcs.ect
```

It is started and supervised by `stlgn.ect`. It does not belong under
`RXFS/Applications/` and it does not link the kernel GUI implementation.

## Source and build outputs

- Source: `user_components/stlcs/stlcs.cpp`
- Shared user/host ABI: `user_components/stlcs/stlcs_abi.hpp`
- ECT linker layout: `user_components/stlcs/stlcs.ld`
- ECT packer: `user_components/stlcs/pack_ect.py`
- ECT validator: `user_components/stlcs/validate_ect.py`
- Build command: `bash scripts/build_stlcs.sh`
- RXFS output: `RXFS/sys/User/stlcs.ect`
- Debug ELF sidecar: `build/user_components/stlcs/stlcs.elf`

The component is compiled for x86-64 freestanding mode at `-O0`, with `-g3`,
no inlining, no LTO and frame pointers. The debug ELF is retained for symbol
and source-level diagnostics. The ECT v1 image contains only loadable sections.

## Host ABI contract

`stlgn.ect` supplies `HostApi` version 1:

- `poll_input`: supplies key-down events.
- `get_clock`: provides local time/date strings.
- `present`: presents the lock-screen model through the system UI/compositor.
- `authenticate`: forwards the supplied credential to `lsass.ect` or its
  protected authentication endpoint.
- `request_desktop`: asks the session manager to begin the desktop session
  after successful authentication.
- `yield`: yields execution to the user-mode scheduler.

The presentation structure contains only asterisks, never the password. The
password is bounded to 63 ASCII bytes, is cleared from session memory before
authentication is called, and the temporary credential copy is wiped after
the callback returns. The authentication callback must not retain the pointer.
The component does not implement or store password hashes and does not decide
whether credentials are valid.

## Current UI behavior

- Starts in the locked state and requests an initial presentation.
- Shows host-provided username, time and date.
- Receives printable ASCII key input; Ctrl/Alt/Meta combinations are ignored.
- Masks entered password characters.
- Backspace removes one character; Escape clears the entry.
- Enter delegates authentication to the host/LSASS path.
- A rejected credential keeps the screen locked and clears the password.
- An unavailable authentication service fails closed.
- The desktop handoff is requested only after authentication returns
  `Accepted`; a failed handoff keeps the component locked.

## Validation completed in the isolated project workspace

- `bash scripts/build_stlcs.sh`: PASS; freestanding x86-64 ELF linked and ECT
  v1 image packaged/structurally validated.
- `bash tests/test_stlcs.sh`: PASS under Clang ASan+UBSan.
- GCC ASan+UBSan: PASS (0 failures).

## Explicit integration limits

This milestone creates the user-mode component and its host ABI. It does **not**
claim that the lock screen can yet run in VMware. Current kernel sources still
have these prerequisites outstanding:

1. `kernel/src/loader/ect_loader.cpp` is still a stub: it checks the `Px` magic
   and returns a fixed address; it does not parse/map ECT sections or start a
   process from `RXFS/sys/User/stlcs.ect`.
2. General-purpose user syscalls and a user-facing graphics/input service ABI
   are not enabled. The Phase 2H DPL3 interrupt is a smoke-test exit gate, not
   a general syscall interface.
3. `stlgn.ect` must implement the HostApi bridge and start this component.
4. `lsass.ect` must provide the authentication endpoint; no password checking
   is implemented in `stlcs.ect`.
5. The present `make_ssp.sh`/`make_disk.sh` flow does not yet stage the RXFS
   directory into a bootable RXFS volume. Adding the file to the source tree
   alone does not put it into a VMware disk image.

Do not enable normal Ring 3 launch or claim a working login screen until the ECT
loader, host ABI bridge, authentication boundary, and RXFS image staging have
been implemented and tested.
