# Starlix OS 1.0 Beta — Phase 2J ECT Loader Foundation

## Scope

Phase 2J replaces the old `Px`-magic-only loader placeholder with a strict,
bounded ECT v1 parser. The parser emits an immutable-style `LoadPlan` with the
validated image base, entry address, section offsets, permissions, and ranges.
It does not allocate physical pages, modify page tables, create processes,
create threads, or enter Ring 3.

## ECT v1 policy

- ELF64 x86-64 images are packaged in the documented 128-byte ECT header.
- At most eight sections are accepted.
- Only `.text`, `.rodata`, and `.data` are accepted.
- All sections must be page-offset aligned, readable, non-empty, in-bounds,
  non-overlapping, and within the bounded 64 MiB image span.
- W^X is mandatory. `.text` is RX, `.rodata` is R, and `.data` is RW.
- The image base must equal the current `USER_IMAGE_BASE` contract.
- ECT v1 imports and relocations are rejected; BSS is not supported by this
  file format because it has no distinct memory-size field.
- CRC32 over `.text`, exact payload coverage, section-table offsets, entry
  point containment, supported version/architecture and reserved bytes are
  checked before a plan is returned.

## API

`StNtInspectExecutable()` validates a buffer and returns a bounded load plan.
`StNtLoadExecutable()` intentionally remains fail-closed and returns zero until
physical-page allocation, per-section mapping, rollback, process/thread setup,
and scheduler admission are implemented as a single safe transaction.

## Tests

Run `bash tests/test_ect_loader_phase2j.sh` for Clang ASan+UBSan parser tests.
Run `CXX=g++ bash tests/test_ect_loader_phase2j.sh` for GCC ASan+UBSan tests.
Run `bash scripts/build_kernel.sh` to compile and link the kernel parser object.

## Not yet verified

No user image has been mapped or executed by this phase. No VMware Lock Screen
runtime is claimed. RXFS disk-image staging, `stlgn.ect` host services, `lsass.ect`
authentication and the compositor/input bridge remain later integration work.
