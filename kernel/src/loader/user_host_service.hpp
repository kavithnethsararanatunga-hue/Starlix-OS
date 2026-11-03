// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

namespace starlix::process { struct StThread; }

namespace starlix::loader::host_service {

/* One lock-screen thread may own the service gate at a time. Binding is made
   before the first IRETQ and cleared only if first-entry setup is rejected. */
bool bind_lock_screen_thread(starlix::process::StThread* thread);
void clear_lock_screen_thread(starlix::process::StThread* thread);
bool service_gate_ready();

} // namespace starlix::loader::host_service

struct starlix_interrupt_return_frame;
extern "C" void starlix_user_service_dispatch(void* frame);
