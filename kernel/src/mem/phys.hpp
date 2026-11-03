// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

struct StarlixBootInfo;

namespace starlix::mem {

/* Initialize from the UEFI memory map preserved in StarlixBootInfo.
   Only conventional RAM below 4 GiB is offered by this 32-bit-DMA allocator.
   The current kernel uses identity-addressed memory; this is not an IOMMU or
   non-identity DMA mapping API. */
bool phys_init(const StarlixBootInfo* boot_info);
bool phys_allocator_ready();

/* Allocate a contiguous run of zeroed 4 KiB physical pages.
   Returns the first physical address, or 0 on invalid count, unavailable
   allocator state, or exhaustion. Current x86-64 Starlix assumes identity
   mapping for this physical range. Allocations are boot-lifetime; no free
   or reclamation API is currently provided. */
uint64_t alloc_phys_pages(uint64_t page_count);

/* Backwards-compatible single-page allocator. */
uint64_t alloc_phys_page();

/* Phase 6C: keep the CPU-dereferenceable address separate from the address
   written into an EHCI descriptor. The current backend is explicitly identity
   mapped; this API does not claim IOMMU or non-identity mapping support yet. */
struct DmaRegion {
    void* cpu_address;
    uint64_t device_address;
    uint64_t length;
    bool valid;
};

/* Allocate and register one zeroed 4 KiB DMA32 page. Regions remain registered
   for the lifetime of the current kernel because DMA-safe reclamation is not
   implemented yet. */
bool dma32_alloc_page(DmaRegion& out_region);

/* Resolve only a range wholly inside a page allocated/registered by this API.
   Checks alignment and the full device-visible 32-bit range; never truncates
   an arbitrary CPU pointer into an EHCI address. */
bool dma32_resolve(const void* cpu_address, uint64_t bytes,
                   uint64_t alignment, uint32_t& out_device_address);

/* Phase 6 completion for the supported x86-64 coherent-memory backend.
   These functions validate that the entire range belongs to a registered DMA
   region before issuing ordering fences. They deliberately fail closed on
   non-x86 builds until a platform cache-maintenance backend is implemented. */
bool dma32_sync_for_device(const void* cpu_address, uint64_t bytes);
bool dma32_sync_for_cpu(const void* cpu_address, uint64_t bytes);

} // namespace starlix::mem
