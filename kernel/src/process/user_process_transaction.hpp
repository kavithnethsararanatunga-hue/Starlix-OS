// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

using StlUserU32 = unsigned int;
using StlUserU64 = unsigned long long;

namespace starlix::process {

constexpr StlUserU32 USER_PROCESS_TX_MAGIC = 0x32505455u; // "UTP2"

enum UserProcessStage : StlUserU32 {
    USER_PROCESS_STAGE_ADDRESS_SPACE = 1u << 0,
    USER_PROCESS_STAGE_IMAGE         = 1u << 1,
    USER_PROCESS_STAGE_USER_STACK    = 1u << 2,
    USER_PROCESS_STAGE_KERNEL_STACK  = 1u << 3,
    USER_PROCESS_STAGE_PROCESS       = 1u << 4,
    USER_PROCESS_STAGE_THREAD        = 1u << 5,
    USER_PROCESS_STAGE_STACK_BOUND   = 1u << 6,
    USER_PROCESS_STAGE_SCHEDULER     = 1u << 7
};

struct UserProcessTransaction {
    StlUserU32 magic;
    StlUserU32 stages;
    StlUserU64 address_space_root;
    StlUserU64 image_cookie;
    StlUserU64 user_stack_cookie;
    StlUserU64 kernel_stack_cookie;
    StlUserU64 process_cookie;
    StlUserU64 thread_cookie;
    StlUserU64 entry_point;
    StlUserU64 user_stack_pointer;
    StlUserU64 kernel_stack_top;
};

struct UserProcessBackend {
    void* context;
    bool (*create_address_space)(void*, StlUserU64*);
    bool (*destroy_address_space)(void*, StlUserU64);
    bool (*map_image)(void*, const void*, StlUserU64, StlUserU64,
                      StlUserU64*, StlUserU64*);
    bool (*unmap_image)(void*, StlUserU64, StlUserU64);
    bool (*allocate_user_stack)(void*, StlUserU64, StlUserU64*, StlUserU64*);
    bool (*release_user_stack)(void*, StlUserU64, StlUserU64);
    bool (*allocate_kernel_stack)(void*, StlUserU64, StlUserU64*, StlUserU64*);
    bool (*release_kernel_stack)(void*, StlUserU64, StlUserU64, StlUserU64);
    bool (*create_process)(void*, const char*, StlUserU64, StlUserU64*);
    bool (*create_thread)(void*, StlUserU64, StlUserU64, StlUserU64,
                          StlUserU64, StlUserU64*);
    bool (*bind_kernel_stack)(void*, StlUserU64, StlUserU64);
    bool (*abort_process)(void*, StlUserU64, StlUserU64);
    bool (*admit_thread)(void*, StlUserU64);
    bool (*remove_thread)(void*, StlUserU64);
};

/* The backend owns every resource represented by a nonzero cookie. On failure
   the transaction rolls back in reverse dependency order. If cleanup fails,
   the residual handle remains populated and may be passed to rollback again. */
bool user_process_create_transactional(
    const char* image_name, const void* image_bytes, StlUserU64 image_size,
    const UserProcessBackend& backend, UserProcessTransaction& out_transaction
);
bool user_process_rollback_transaction(
    const UserProcessBackend& backend, UserProcessTransaction& transaction
);

} // namespace starlix::process
