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

#include "ect_format.hpp"
#include "ect_memory_map.hpp"

namespace starlix::mem { struct AddressSpace; }

namespace starlix::loader {

void loader_init();
/* Strictly validate an ECT v1 image and return a bounded load plan.
   This phase performs no page allocation, mapping, process creation or launch. */
bool StNtInspectExecutable(const void* ect_buffer, uint64_t size,
                           ect::LoadPlan* out_plan);
/* Phase 2K: map a validated ECT image into an inactive address space. This
   returns a mapping handle but does not create a process, thread, or launch it. */
bool StNtMapExecutable(const void* ect_buffer, uint64_t size,
                       starlix::mem::AddressSpace* target_space,
                       ect::MappedImage* out_mapping);
/* Undo a Phase 2K mapping and release its managed physical runs. */
bool StNtUnmapExecutable(starlix::mem::AddressSpace* target_space,
                         ect::MappedImage* mapping);
/* Process/thread creation and Ring-3 launch remain deliberately disabled. */
uint64_t StNtLoadExecutable(const void* ect_buffer, uint64_t size);

} // namespace starlix::loader
