// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "user_process_transaction.hpp"

namespace starlix::process {
namespace {

bool valid_name(const char* name) {
    if (!name) return false;
    for (StlUserU32 i = 0u; i < 32u; ++i) {
        if (name[i] == '\0') return i != 0u;
    }
    return false;
}

bool backend_complete(const UserProcessBackend& b) {
    return b.create_address_space && b.destroy_address_space &&
        b.map_image && b.unmap_image && b.allocate_user_stack &&
        b.release_user_stack && b.allocate_kernel_stack &&
        b.release_kernel_stack && b.create_process && b.create_thread &&
        b.bind_kernel_stack && b.abort_process && b.admit_thread &&
        b.remove_thread;
}

void clear_transaction(UserProcessTransaction& tx) {
    tx = UserProcessTransaction{};
}

} // namespace

bool user_process_rollback_transaction(
    const UserProcessBackend& backend, UserProcessTransaction& tx
) {
    if (!backend_complete(backend) || tx.magic != USER_PROCESS_TX_MAGIC)
        return false;

    if ((tx.stages & USER_PROCESS_STAGE_SCHEDULER) != 0u) {
        if (!tx.thread_cookie || !backend.remove_thread(
                backend.context, tx.thread_cookie)) return false;
        tx.stages &= ~USER_PROCESS_STAGE_SCHEDULER;
    }

    if ((tx.stages & USER_PROCESS_STAGE_KERNEL_STACK) != 0u) {
        if (!tx.kernel_stack_cookie || !backend.release_kernel_stack(
                backend.context, tx.address_space_root,
                tx.kernel_stack_cookie,
                (tx.stages & USER_PROCESS_STAGE_THREAD) ? tx.thread_cookie : 0ULL)) {
            return false;
        }
        tx.stages &= ~(USER_PROCESS_STAGE_KERNEL_STACK |
                       USER_PROCESS_STAGE_STACK_BOUND);
        tx.kernel_stack_cookie = 0ULL;
        tx.kernel_stack_top = 0ULL;
    }

    if ((tx.stages & USER_PROCESS_STAGE_PROCESS) != 0u) {
        if (!tx.process_cookie || !backend.abort_process(
                backend.context, tx.process_cookie,
                (tx.stages & USER_PROCESS_STAGE_THREAD) ? tx.thread_cookie : 0ULL)) {
            return false;
        }
        tx.stages &= ~(USER_PROCESS_STAGE_PROCESS | USER_PROCESS_STAGE_THREAD);
        tx.process_cookie = 0ULL;
        tx.thread_cookie = 0ULL;
    }

    if ((tx.stages & USER_PROCESS_STAGE_USER_STACK) != 0u) {
        if (!tx.user_stack_cookie || !backend.release_user_stack(
                backend.context, tx.address_space_root,
                tx.user_stack_cookie)) return false;
        tx.stages &= ~USER_PROCESS_STAGE_USER_STACK;
        tx.user_stack_cookie = 0ULL;
        tx.user_stack_pointer = 0ULL;
    }

    if ((tx.stages & USER_PROCESS_STAGE_IMAGE) != 0u) {
        if (!tx.image_cookie || !backend.unmap_image(
                backend.context, tx.address_space_root, tx.image_cookie))
            return false;
        tx.stages &= ~USER_PROCESS_STAGE_IMAGE;
        tx.image_cookie = 0ULL;
        tx.entry_point = 0ULL;
    }

    if ((tx.stages & USER_PROCESS_STAGE_ADDRESS_SPACE) != 0u) {
        if (!tx.address_space_root || !backend.destroy_address_space(
                backend.context, tx.address_space_root)) return false;
        tx.stages &= ~USER_PROCESS_STAGE_ADDRESS_SPACE;
        tx.address_space_root = 0ULL;
    }

    clear_transaction(tx);
    return true;
}

bool user_process_create_transactional(
    const char* image_name, const void* image_bytes, StlUserU64 image_size,
    const UserProcessBackend& backend, UserProcessTransaction& out_transaction
) {
    clear_transaction(out_transaction);
    if (!valid_name(image_name) || !image_bytes || image_size == 0ULL ||
        !backend_complete(backend)) return false;

    UserProcessTransaction tx{};
    tx.magic = USER_PROCESS_TX_MAGIC;

    bool ok = backend.create_address_space(
        backend.context, &tx.address_space_root);
    if (tx.address_space_root != 0ULL)
        tx.stages |= USER_PROCESS_STAGE_ADDRESS_SPACE;
    if (!ok || tx.address_space_root == 0ULL) goto fail;

    ok = backend.map_image(backend.context, image_bytes, image_size,
                           tx.address_space_root, &tx.entry_point,
                           &tx.image_cookie);
    if (tx.image_cookie != 0ULL) tx.stages |= USER_PROCESS_STAGE_IMAGE;
    if (!ok || tx.image_cookie == 0ULL || tx.entry_point == 0ULL) goto fail;

    ok = backend.allocate_user_stack(
        backend.context, tx.address_space_root, &tx.user_stack_pointer,
        &tx.user_stack_cookie);
    if (tx.user_stack_cookie != 0ULL)
        tx.stages |= USER_PROCESS_STAGE_USER_STACK;
    if (!ok || tx.user_stack_cookie == 0ULL ||
        tx.user_stack_pointer == 0ULL) goto fail;

    ok = backend.allocate_kernel_stack(
        backend.context, tx.address_space_root, &tx.kernel_stack_top,
        &tx.kernel_stack_cookie);
    if (tx.kernel_stack_cookie != 0ULL)
        tx.stages |= USER_PROCESS_STAGE_KERNEL_STACK;
    if (!ok || tx.kernel_stack_cookie == 0ULL ||
        tx.kernel_stack_top == 0ULL) goto fail;

    ok = backend.create_process(backend.context, image_name,
                                tx.address_space_root, &tx.process_cookie);
    if (tx.process_cookie != 0ULL) tx.stages |= USER_PROCESS_STAGE_PROCESS;
    if (!ok || tx.process_cookie == 0ULL) goto fail;

    ok = backend.create_thread(backend.context, tx.process_cookie,
                               tx.entry_point, tx.user_stack_pointer,
                               tx.kernel_stack_top, &tx.thread_cookie);
    if (tx.thread_cookie != 0ULL) tx.stages |= USER_PROCESS_STAGE_THREAD;
    if (!ok || tx.thread_cookie == 0ULL) goto fail;

    if (!backend.bind_kernel_stack(backend.context, tx.kernel_stack_cookie,
                                   tx.thread_cookie)) goto fail;
    tx.stages |= USER_PROCESS_STAGE_STACK_BOUND;

    if (!backend.admit_thread(backend.context, tx.thread_cookie)) goto fail;
    tx.stages |= USER_PROCESS_STAGE_SCHEDULER;

    out_transaction = tx;
    return true;

fail:
    if (!user_process_rollback_transaction(backend, tx))
        out_transaction = tx; // Preserve an exact residual-resource handle.
    return false;
}

} // namespace starlix::process
