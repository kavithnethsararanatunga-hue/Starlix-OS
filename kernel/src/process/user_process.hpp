// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#pragma once

#include "process.hpp"

namespace starlix::process {

constexpr uint32_t USER_PROCESS_HANDLE_MAGIC = 0x32504855u; // "UHP2"

struct UserProcessHandle {
    uint32_t magic;
    uint32_t reserved;
    uint64_t resource_cookie;
    uint64_t pid;
    uint64_t tid;
    uint64_t address_space_root;
    uint64_t entry_point;
    uint64_t user_stack_pointer;
};

/* Build an inactive address space, map ECT image + writable/NX user stack,
   allocate/bind a guarded kernel stack, register process/thread, then admit
   its READY main thread to the scheduler. This does not enter Ring 3. */
bool StNtCreateUserProcessFromECT(const char* image_name,
                                  const void* ect_bytes,
                                  uint64_t ect_size,
                                  UserProcessHandle* out_handle);

/* Enter the prepared main thread through the validated IRETQ first-user path.
   On a successful launch this routine does not return; false means the launch
   was rejected and the process remains READY/abortable. */
bool StNtStartUserProcess(const UserProcessHandle* handle);

/* Undo a prepared process before it has started running. If cleanup fails,
   the handle remains valid and this function may be retried. */
bool StNtAbortUserProcess(UserProcessHandle* handle);
StThread* user_process_main_thread(const UserProcessHandle* handle);

} // namespace starlix::process
