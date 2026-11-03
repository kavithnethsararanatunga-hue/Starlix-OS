# Starlix stbootmngr boot-flow Phase 8

## What changes

`stbtmngr.efi` now tries the new RXFS-first route before the transitional SSP route:

1. Enumerate UEFI Disk I/O handles and mount an RXFS volume read-only.
2. Open `sys/BaseSystem.dmg` through RXFS.
3. Resolve the UDIF Apple_APFS partition, APFS volume, and `/System/Library/CoreServices/boot.efi` bytes.
4. Load the file as a UEFI image with `LoadImage()`.
5. Put a versioned `STBOOT_FS_HANDOFF` pointer in the child loaded-image protocol's `LoadOptions`, then call `StartImage()`.

If the RXFS/BaseSystem path cannot prepare or load an image, the old SSP -> `Rxload.efi` route remains as a transitional fallback. If `boot.efi` was started and returns, the manager halts rather than returning to SSP, because the child may have altered boot state.

## Starlix filesystem handoff ABI

`STBOOT_FS_HANDOFF` has signature `STBFS001`, version 1, and three callbacks: `Lookup`, `GetInfo`, and `Read`. Paths are absolute UTF-8 strings and matched byte-for-byte. The APIs are read-only and serialized/non-reentrant. The callbacks and context live in the parent `stbtmngr.efi` image, so the child must use them while the parent remains loaded and before `ExitBootServices()`.

The child `boot.efi` is expected to obtain its own `EFI_LOADED_IMAGE_PROTOCOL`, validate `LoadOptionsSize`, signature, version and struct size, and only then use the handoff. Kernel and Starlix-format extension loading remains the responsibility of `boot.efi`, not `stbtmngr.efi`.

## Safety and limitations

- `main.cpp` now has a primary RXFS-first path, but it is not yet validated against a real Starlix BaseSystem image in a VM.
- The existing SSP route is intentionally retained as fallback and has not been removed.
- The readers currently support only a bounded subset of UDIF/APFS features. Unsupported codecs, encryption, malformed metadata, and unsupported APFS layouts fail closed.
- No physical EFI/SSP partition or other disk is written by this patch. No `ExitBootServices()` call is introduced here.
- `LoadImage()` must be able to recognize the `boot.efi` bytes as an EFI PE/COFF image; if that fails, the loader goes to the transitional fallback.
