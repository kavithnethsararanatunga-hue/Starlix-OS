// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once
#include "../ob/object.hpp"

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::ipc {

struct PORT_MESSAGE {
    uint16_t data_length;
    uint16_t total_length;
    uint32_t message_type;
    uint64_t client_pid;
    uint64_t message_id;
};

struct AlpcPort {
    starlix::ob::ObjectHeader header;
    char port_name[32];
    uint64_t owner_pid;
    bool connected;
};

void alpc_init();
starlix::ob::HANDLE StNtCreatePort(const char* name);
uint64_t StNtRequestWaitReplyPort(starlix::ob::HANDLE port_handle, PORT_MESSAGE* msg_in, PORT_MESSAGE* msg_out);

} // namespace starlix::ipc
