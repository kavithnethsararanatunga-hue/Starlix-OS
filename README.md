# STARLIX OS

**STARLIX OS 1.0.Beta** is an experimental x86-64 operating system project.

This repository contains selected first-party source code, UEFI bootloader
sources, kernel sources, filesystem tools, user components, documentation,
and development tests.

## Project structure

- `boot/` — UEFI bootloader source and assets.
- `kernel/src/` — kernel implementation.
- `include/` — shared interfaces and ABI headers.
- `rxfs/` — RXFS format and filesystem tools.
- `user_apps/` — user application sources.
- `user_components/` — user-space components.
- `user_lib/` — user-space library sources.
- `system_root/` — system components and applications.
- `scripts/` — build and image scripts.
- `tests/` — host-side tests and source contract checks.
- `tools/` — project utilities.
- `docs/` — technical documentation.

## Build

Build the kernel using:

    bash scripts/build_kernel.sh

Build requirements depend on the selected target and toolchain.

## Selected verification results

The following checks were observed in the development environment:

- MMIO/PAT policy tests: 31 checks passed with Clang.
- MMIO/PAT policy tests: 31 checks passed with GCC.
- Address-space regression tests: 116 checks passed with Clang.
- Address-space regression tests: 116 checks passed with GCC.
- Kernel build completed successfully.
- The specifically audited Intel/GPU implementation symbols were absent
  from the kernel ELF.

These results describe the tested development build. They do not establish
complete hardware compatibility or successful operation on every physical
machine.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).

Copyright (c) 2026 Kavith Ranatunga.
