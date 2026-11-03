// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "phys.hpp"

namespace starlix::mem {

/* Managed pages are allocated from the existing identity-mapped physical
   allocator, but additionally tracked so a failed user-image transaction can
   safely return its exact pages for later reuse. */
bool managed_page_pool_init();
bool managed_page_pool_ready();
uint64_t managed_alloc_phys_pages(uint64_t page_count);
bool managed_release_phys_pages(uint64_t physical_base, uint64_t page_count);
uint64_t managed_alloc_phys_page();
bool managed_release_phys_page(uint64_t physical_page);

} // namespace starlix::mem
