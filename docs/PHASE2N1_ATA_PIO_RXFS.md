# Starlix OS 1.0 Beta — Phase 2N.1 ATA PIO and RXFS runtime

## Scope

This patch makes the kernel storage registry able to read a VMware IDE disk
without depending on an EHCI controller. The new ATA backend is read-only.
The existing EHCI USB Mass Storage backend remains available through the same
block-device registry.

This is **not** the full Phase 2N production Lock Screen implementation. The
user-mode HostApi bridge, trusted authentication/display/input services, and
component termination/cleanup lifecycle are still separate requirements.

## VMware evidence used for controller selection

The supplied Phase 2N candidate VMX attaches the virtual disk as `ide0:0`.
Its captured PCI log shows an Intel IDE controller with PCI identity
`8086:7111`, class `01`, subclass `01`, programming interface `8A` (observed at
BDF `00:07.1`). ProgIF `8A` selects compatibility-mode primary and secondary
IDE channels, so the ATA driver uses the standard legacy task-file ports after
checking PCI I/O decoding is enabled:

- Primary command/control: `0x1F0` / `0x3F6`
- Secondary command/control: `0x170` / `0x376`

The driver does not assume that every IDE controller uses these ports. It
parses the PCI programming interface and validates native-mode I/O BARs when
a channel is in native mode.

## Implementation

- `kernel/src/drivers/ata_pio.cpp` discovers PCI IDE controllers, probes master
  and slave devices with ATA IDENTIFY, and stores validated device geometry.
- LBA28 `READ SECTORS` and LBA48 `READ SECTORS EXT` are supported. Polling is
  bounded, ATA interrupts are masked for the polling transaction, sector size
  is restricted to 512–4096 bytes, and each request is limited to 4096 bytes.
- No ATA write or DMA command is implemented. ATA requests dispatched to the
  generic write/flush APIs fail closed.
- `storage.cpp` registers ATA PIO devices and reuses the existing GPT/MBR,
  RXFS and FAT32 readers. EHCI USB MSC devices still use the same registry.
- `main.cpp` invokes `storage_init()` regardless of whether `ehci_init()`
  succeeds. USB remains optional; IDE discovery no longer depends on USB.
- `scripts/build_kernel.sh` compiles and links `ata_pio.cpp` exactly once.
  `storage.cpp` remains included by `main.cpp` and is not separately compiled.

## Phase 12 RXFS fixture correction

The prior synthetic Phase 12 RXFS fixture contained `sys/BaseSystem.dmg`, but
not `sys/User/stlcs.ect`. A successful ATA backend alone would therefore have
advanced the boot log from `RXFS_NOT_MOUNTED` to `COMPONENT_NOT_FOUND`.

The fixture exporter now accepts an optional real `stlcs.ect` input. The
Phase 2N.1 candidate builder supplies the repository component, adds the
`User` directory and component inode to the RXFS volume, and verifies that the
UEFI RXFS reader can open `/sys/User/stlcs.ect` and read every byte back
unchanged. The current fixture intentionally caps this file at one 4 KiB RXFS
block; the supplied component is 3342 bytes.

The fixture's last block remains reserved for the RXFS backup superblock. The
new User directory and component data are placed in the final two data blocks
before that reserved block, outside the synthetic BaseSystem.dmg mapping.

## Tests

- `tests/test_ata_pio_policy.sh`: channel-mode, PCI I/O BAR, IDENTIFY capacity,
  sector-size, and read-bound policy tests with Clang/GCC ASan+UBSan.
- `tests/test_ata_pio_driver_contract.sh`: confirms PCI IDE discovery, IDENTIFY,
  bounded polling, LBA28/LBA48 reads, read-only ATA policy, generic storage
  dispatch, EHCI-independent initialization, and build integration.
- `tests/test_phase2n1_stlcs_ect_validation.sh`: validates the actual
  `stlcs.ect` using the same ECT v1 parser linked into the kernel, under
  Clang/GCC ASan+UBSan.
- `tests/test_phase2n1_rxfs_stlcs_fixture.sh`: creates a disposable synthetic
  RXFS fixture and verifies exact component lookup and byte-for-byte readback.

## Running a normal Phase 2N.1 candidate

From WSL Ubuntu, in the Starlix project root:

```bash
bash tools/build_starlix_phase2n1_candidate.sh \
  "$PWD/build/candidate_rxfs_phase10/run_20261027_215640"
```

Pass another Phase 10 run directory if the original path is different. The
script backs up generated kernel/EFI artifacts, builds the normal production
kernel (Ring 3 diagnostic disabled), checks undefined symbols and debug info,
packages the kernel ECT, runs the Phase 2N.1 host validations, then invokes the
existing disposable Phase 12 candidate pipeline. The original Phase 10 image
is used as input and is not modified.

## Expected runtime evidence and limitations

A successful VMware test should show ATA controller discovery, successful
IDENTIFY, a validated read-only block device, RXFS mount, Lock Screen file
lookup, and ECT v1 validation. Since the HostApi bridge is not part of this
milestone, the expected safe stopping reason after successful validation is
`USER_HOST_SERVICE_BRIDGE_UNAVAILABLE`.

The uploaded source package contained the VMX/serial reference but not the
actual Phase 10 VMDK/IMG, and this build environment has no VMware runtime.
Therefore no live ATA PIO transaction or VMware boot is claimed here. The
hardware acceptance step must be run on the user's Windows/WSL + VMware setup.
