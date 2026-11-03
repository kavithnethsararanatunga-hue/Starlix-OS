# STARLIX Intel GPU KEXT source boundary

Authoritative Intel GPU KEXT C++ source is stored here:

- `intlgpudvs.cpp`
- `intel_gpu_pci_policy.hpp`

The source is compiled only by `scripts/build_intlgpudvs.sh` into the separate
ELF relocatable object, then packaged as `Rx/Drivers/Gpu/BasicDriver/Intel/intlgpudvs.sys`
and placed in the `intlgpudvs.kext` bundle.

`kernel/src/drivers/Gpu/Basic/Intel` is a source organization path only. The
kernel build has an explicit compile/link list and must never compile or link
these Intel driver translation units. The generic kernel PCI layer enumerates
PCI devices without Intel-specific matching. PCI identity matching belongs to
the external Intel KEXT.

The generic kernel-to-driver PCI descriptor is defined in
`include/driver/pci_abi.hpp`. The external GPU KEXT entry/host context is defined
in `include/driver/gpu_kext_abi.hpp`. The kernel snapshot API only reads PCI
configuration space; it does not size BARs by writing configuration registers.

The packaged entry symbol is `StarlixGpuKextEntry`. It validates the supplied
PCI descriptor for Intel HD Graphics 2500 (`8086:0152`) and initializes the
existing GOP framebuffer graphics API. This is a software framebuffer backend,
not Intel hardware acceleration.

Native Intel Gen7 MMIO acceleration, modesetting, command submission, IRQ/VSync,
resource ownership, executable KEXT page mapping and runtime KEXT entry
execution are not yet implemented or claimed by this phase.
