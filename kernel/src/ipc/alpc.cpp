// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "alpc.hpp"

namespace starlix::ipc {

static AlpcPort system_alpc_ports[16];
static size_t port_count = 0;

void alpc_init() {
    port_count = 0;
}

starlix::ob::HANDLE StNtCreatePort(const char* name) {
    if (port_count >= 16) return starlix::ob::ST_INVALID_HANDLE_VALUE;

    AlpcPort* port = &system_alpc_ports[port_count++];
    port->header.ref_count = 1;
    port->header.handle_count = 1;
    port->header.type = starlix::ob::OB_TYPE_EVENT;

    size_t i = 0;
    for (; i < 31 && name[i]; i++) port->port_name[i] = name[i];
    port->port_name[i] = 0;

    port->owner_pid = 4; // System Process
    port->connected = true;

    return starlix::ob::ObInsertObject(port, starlix::ob::OB_TYPE_EVENT, starlix::ob::ST_GENERIC_READ | starlix::ob::ST_GENERIC_WRITE);
}

uint64_t StNtRequestWaitReplyPort(starlix::ob::HANDLE port_handle, PORT_MESSAGE* msg_in, PORT_MESSAGE* msg_out) {
    if (!port_handle || !msg_in || !msg_out) return 0xFFFFFFFFFFFFFFFFULL;
    msg_out->message_id = msg_in->message_id;
    msg_out->client_pid = msg_in->client_pid;
    msg_out->message_type = 1; // ACK_SUCCESS
    return 0; // STATUS_SUCCESS
}

} // namespace starlix::ipc
