// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::mem {

constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t PAGE_PRESENT     = 1ULL << 0;
constexpr uint64_t PAGE_WRITABLE    = 1ULL << 1;
constexpr uint64_t PAGE_USER        = 1ULL << 2;

void vmm_init();
/* Audits inherited four-level tables read-only, then enables only EFER.NXE. */
bool vmm_enable_nxe();
const char* vmm_nxe_failure_reason();
uint64_t StNtAllocateVirtualMemory(uint64_t size, uint32_t protect_flags);

} // namespace starlix::mem
