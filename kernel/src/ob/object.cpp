// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "object.hpp"

namespace starlix::ob {

constexpr size_t MAX_HANDLES = 256;
static HandleTableEntry system_handle_table[MAX_HANDLES];

void ob_init() {
    for (size_t i = 0; i < MAX_HANDLES; i++) {
        system_handle_table[i].object_ptr = nullptr;
        system_handle_table[i].granted_access = 0;
    }
}

HANDLE ObInsertObject(void* object_body, ObjectType type, uint32_t access) {
    if (!object_body) return ST_INVALID_HANDLE_VALUE;

    ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(
        reinterpret_cast<uint8_t*>(object_body) - sizeof(ObjectHeader)
    );
    hdr->type = type;
    hdr->ref_count++;
    hdr->handle_count++;

    for (size_t i = 1; i < MAX_HANDLES; i++) {
        if (system_handle_table[i].object_ptr == nullptr) {
            system_handle_table[i].object_ptr = object_body;
            system_handle_table[i].granted_access = access;
            return static_cast<HANDLE>(i * 4); // NT Handles are multiples of 4
        }
    }
    return ST_INVALID_HANDLE_VALUE;
}

void* ObReferenceObjectByHandle(HANDLE handle, ObjectType type, uint32_t required_access) {
    uint64_t index = handle / 4;
    if (index == 0 || index >= MAX_HANDLES) return nullptr;

    HandleTableEntry& entry = system_handle_table[index];
    if (!entry.object_ptr) return nullptr;

    if ((entry.granted_access & required_access) != required_access) return nullptr;

    ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(
        reinterpret_cast<uint8_t*>(entry.object_ptr) - sizeof(ObjectHeader)
    );

    if (hdr->type != type) return nullptr;

    hdr->ref_count++;
    return entry.object_ptr;
}

void ObDereferenceObject(void* object_body) {
    if (!object_body) return;
    ObjectHeader* hdr = reinterpret_cast<ObjectHeader*>(
        reinterpret_cast<uint8_t*>(object_body) - sizeof(ObjectHeader)
    );
    if (hdr->ref_count > 0) {
        hdr->ref_count--;
    }
}

} // namespace starlix::ob
