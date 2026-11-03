# Starlix OS 1.0 Beta — Phase 2K ECT Memory-Mapping Transaction

## Scope

Phase 2K extends the Phase 2J ECT v1 parser with a transactional image-section
mapper. A validated ECT image can now be copied into managed physical pages and
mapped into a registered, inactive `AddressSpace`. This phase does **not**
create a `StProcess`, create a user thread, admit a scheduler candidate, or
enter Ring 3. `StNtLoadExecutable()` therefore remains fail-closed and returns
zero; callers use `StNtMapExecutable()` only to obtain a mapping handle.

## Added/updated source files

- `kernel/src/mem/managed_page_pool.hpp/.cpp`
  - Tracks page leases acquired from the existing UEFI conventional-memory
    allocator.
  - Supports exact-run release, detects double/partial/foreign releases, and
    coalesces freed extents so contiguous runs can be reused.
  - Zeroes every page before it is returned to a caller.
  - Bounded registry: 32,768 unique pages (128 MiB) and 4,096 free extents.
- `kernel/src/mem/address_space.hpp/.cpp`
  - Adds an optional physical-page release callback to the address-space
    platform adapter.
  - The kernel adapter now allocates address-space table pages from the
    managed page pool.
- `kernel/src/mem/address_space_core.cpp`
  - Releases newly allocated table pages when table mapping fails.
  - Reclaims empty user PT/PD/PDPT levels after a final leaf unmap.
  - Prunes empty intermediate tables after a partially failed map operation.
  - The existing contract still forbids editing an active address space.
- `kernel/src/loader/ect_memory_map.hpp/.cpp`
  - Validates the complete ECT image before allocating anything.
  - Allocates a contiguous page run per section, zeroes the run, copies the
    section bytes, and maps one user page per 4 KiB frame.
  - Retains ECT section permissions (`.text` RX, `.rodata` R, `.data` RW); W^X
    remains mandatory.
  - Rolls back all successful mappings and page leases on allocation/map
    failure. If cleanup itself fails, returns a residual `MappedImage` handle
    so cleanup can be retried without releasing still-mapped frames.
  - Binds each mapping handle to the owner address-space CR3, preventing a
    handle from being used to unmap another process's same-VA pages.
- `kernel/src/loader/ect_loader.hpp/.cpp`
  - Adds `StNtMapExecutable()` and `StNtUnmapExecutable()` adapters for a
    registered inactive address space.
  - Keeps `StNtLoadExecutable()` fail-closed until process/thread creation and
    Ring-3 launch integration are implemented.
- `scripts/build_kernel.sh`
  - Compiles and links the managed page pool and ECT memory mapper.

## Mapping invariants

1. The ECT parser must accept the entire image before allocation begins.
2. Each section virtual base remains the validated ECT address; all mapped
   frames are 4 KiB aligned and identity-accessible below 4 GiB in the current
   kernel memory model.
3. Every mapped leaf is user-accessible and has the exact section permissions.
   Writable+executable mappings are rejected.
4. A physical run is released only after all of its mapped pages have been
   successfully unmapped.
5. A managed allocation can only be released as the exact run originally
   leased; partial, double, unaligned, or foreign release attempts fail.
6. Empty user page-table levels are detached before their managed pages are
   returned to the pool.
7. The target address space must be registered and inactive. Mapping handles
   carry the target CR3 and reject cross-address-space unmap attempts.

## Tests

- `tests/test_managed_page_pool_phase2k.sh`
  - Exact lease release, partial/double/unaligned release rejection, contiguous
    extent coalescing/reuse, and zero-on-reuse.
- `tests/test_address_space_phase2k_reclaim.sh`
  - PT/PD/PDPT reclamation and rollback after an intermediate table allocation
    failure.
- `tests/test_ect_memory_map_phase2k.sh`
  - Valid-section copy, zero-filled page tail, RX/R permissions, mapping and
    allocation failure rollback, owner-bound unmap, retry after an injected
    unmap failure, and invalid-image rejection before allocation.

All host tests are built with AddressSanitizer and UndefinedBehaviorSanitizer.

## Current limitations

- The active hardware physical allocator remains a bump allocator for new RAM;
  the managed pool can reuse pages that were explicitly released through this
  Phase 2K lease API.
- The current physical-memory model is identity mapped and constrained below
  4 GiB. No high-memory direct-map abstraction, IOMMU, or SMP shootdown is added.
- Address-space/page-table map APIs require the target root to be inactive.
- The mapper has no filesystem reader; the caller must already have the full
  ECT image in kernel-readable memory.
- No user stack/arguments, `StProcess`, `StThread`, scheduler admission,
  `stlgn.ect` host services, LSASS authentication, or actual Lock Screen launch
  is included in Phase 2K.

## Build

```bash
bash tests/test_managed_page_pool_phase2k.sh
CXX=g++ bash tests/test_managed_page_pool_phase2k.sh
bash tests/test_address_space_phase2k_reclaim.sh
CXX=g++ bash tests/test_address_space_phase2k_reclaim.sh
bash tests/test_ect_memory_map_phase2k.sh
CXX=g++ bash tests/test_ect_memory_map_phase2k.sh
bash scripts/build_kernel.sh
```
