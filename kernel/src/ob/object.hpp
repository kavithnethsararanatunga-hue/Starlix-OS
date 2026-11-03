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

namespace starlix::ob {

using HANDLE = uint64_t;
constexpr HANDLE ST_INVALID_HANDLE_VALUE = 0;

enum ObjectType : uint32_t {
    OB_TYPE_PROCESS = 1,
    OB_TYPE_THREAD  = 2,
    OB_TYPE_FILE    = 3,
    OB_TYPE_SECTION = 4,
    OB_TYPE_EVENT   = 5
};

enum AccessMask : uint32_t {
    ST_GENERIC_READ    = 0x80000000,
    ST_GENERIC_WRITE   = 0x40000000,
    ST_GENERIC_EXECUTE = 0x20000000,
    ST_GENERIC_ALL     = 0x10000000
};

struct ObjectHeader {
    uint64_t ref_count;
    uint64_t handle_count;
    ObjectType type;
    uint32_t flags;
};

struct HandleTableEntry {
    void* object_ptr;
    uint32_t granted_access;
};

void ob_init();
HANDLE ObInsertObject(void* object_body, ObjectType type, uint32_t access);
void* ObReferenceObjectByHandle(HANDLE handle, ObjectType type, uint32_t required_access);
void ObDereferenceObject(void* object_body);

} // namespace starlix::ob
