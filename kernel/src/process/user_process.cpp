// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include "user_process.hpp"
#include "user_process_transaction.hpp"
#include "user_process_start_policy.hpp"
#include "scheduler.hpp"
#include "context_switch.hpp"
#include "../loader/ect_loader.hpp"
#include "../mem/address_space.hpp"
#include "../mem/kernel_stack.hpp"
#include "../mem/managed_page_pool.hpp"
#include "../mem/phys.hpp"
#include "../mem/user_address.hpp"

namespace starlix::process {
namespace {

using namespace starlix::mem::user_address;
constexpr uint32_t USER_PROCESS_RECORD_CAPACITY = 16u;
constexpr uint64_t USER_STACK_ENTRY_RESERVE = 16ULL;
constexpr uint64_t IDENTITY_LIMIT = 0x100000000ULL;

struct UserProcessRecord {
    bool used;
    uint64_t cookie;
    starlix::mem::AddressSpace address_space;
    starlix::loader::ect::MappedImage image;
    starlix::mem::KernelStack kernel_stack;
    uint64_t user_stack_physical;
    uint32_t user_stack_mapped_pages;
    bool user_stack_leased;
    StProcess* process;
    StThread* thread;
    bool launch_committed;
    UserProcessTransaction transaction;
};

static UserProcessRecord g_records[USER_PROCESS_RECORD_CAPACITY]{};
static uint32_t g_record_lock = 0u;
static uint64_t g_next_cookie = 1ULL;

void lock_records() {
    for (;;) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&g_record_lock, &expected, 1u, false,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        asm volatile("pause" ::: "memory");
    }
}
void unlock_records() {
    __atomic_store_n(&g_record_lock, 0u, __ATOMIC_RELEASE);
}

uint64_t irq_save_disable() {
    uint64_t flags = 0ULL;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
void irq_restore(uint64_t flags) {
    if ((flags & (1ULL << 9)) != 0ULL) asm volatile("sti" ::: "memory");
}

void reset_record_payload(UserProcessRecord& record) {
    record.address_space = starlix::mem::AddressSpace{};
    record.image = starlix::loader::ect::MappedImage{};
    record.kernel_stack = starlix::mem::KernelStack{};
    record.user_stack_physical = 0ULL;
    record.user_stack_mapped_pages = 0u;
    record.user_stack_leased = false;
    record.process = nullptr;
    record.thread = nullptr;
    record.launch_committed = false;
    record.transaction = UserProcessTransaction{};
}

UserProcessRecord* find_record(uint64_t cookie) {
    if (cookie == 0ULL) return nullptr;
    for (uint32_t i = 0u; i < USER_PROCESS_RECORD_CAPACITY; ++i)
        if (g_records[i].used && g_records[i].cookie == cookie)
            return &g_records[i];
    return nullptr;
}

bool create_address_space(void* context, uint64_t* out_root) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !out_root) return false;
    *out_root = 0ULL;
    record->address_space = starlix::mem::AddressSpace{};
    if (!starlix::mem::address_space_create(record->address_space)) return false;
    *out_root = record->address_space.cr3;
    return *out_root != 0ULL;
}

bool destroy_address_space(void* context, uint64_t root) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || root == 0ULL || record->address_space.cr3 != root)
        return false;
    if (!starlix::mem::address_space_destroy(record->address_space)) return false;
    return true;
}

bool map_image(void* context, const void* bytes, uint64_t size,
               uint64_t root, uint64_t* out_entry, uint64_t* out_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !bytes || size == 0ULL || !out_entry || !out_cookie ||
        record->address_space.cr3 != root) return false;
    *out_entry = 0ULL;
    *out_cookie = 0ULL;
    const bool mapped = starlix::loader::StNtMapExecutable(
        bytes, size, &record->address_space, &record->image);
    if (record->image.magic == starlix::loader::ect::MAPPED_IMAGE_MAGIC) {
        *out_cookie = reinterpret_cast<uint64_t>(&record->image);
        *out_entry = record->image.entry_point;
    }
    return mapped && *out_cookie != 0ULL && *out_entry != 0ULL;
}

bool unmap_image(void* context, uint64_t root, uint64_t cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || record->address_space.cr3 != root ||
        cookie != reinterpret_cast<uint64_t>(&record->image)) return false;
    return starlix::loader::StNtUnmapExecutable(
        &record->address_space, &record->image);
}

bool allocate_user_stack(void* context, uint64_t root,
                         uint64_t* out_stack_pointer, uint64_t* out_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !out_stack_pointer || !out_cookie ||
        record->address_space.cr3 != root || record->user_stack_leased ||
        record->user_stack_physical != 0ULL) return false;
    *out_stack_pointer = 0ULL;
    *out_cookie = 0ULL;

    const uint64_t physical = starlix::mem::managed_alloc_phys_pages(USER_STACK_PAGES);
    if (physical == 0ULL) return false;
    record->user_stack_physical = physical;
    record->user_stack_leased = true;
    *out_cookie = physical; // Preserve ownership even if a later map fails.

    if (physical >= IDENTITY_LIMIT ||
        USER_STACK_BYTES > IDENTITY_LIMIT - physical) return false;

    for (uint64_t page = 0ULL; page < USER_STACK_PAGES; ++page) {
        const uint64_t va = USER_STACK_BASE + page * PAGE_SIZE;
        const uint64_t pa = physical + page * PAGE_SIZE;
        if (!starlix::mem::address_space_map_user_page(
                record->address_space, va, pa,
                USER_PAGE_READ | USER_PAGE_WRITE)) return false;
        ++record->user_stack_mapped_pages;
    }

    /* Startup stack is 16-byte aligned and contains an empty, deterministic
       two-qword bootstrap area. The initial ECT ABI is documented separately
       from the later POSIX process-entry ABI. */
    volatile uint64_t* initial = reinterpret_cast<volatile uint64_t*>(
        static_cast<uintptr_t>(physical + USER_STACK_BYTES - USER_STACK_ENTRY_RESERVE));
    initial[0] = 0ULL;
    initial[1] = 0ULL;
    *out_stack_pointer = USER_STACK_TOP - USER_STACK_ENTRY_RESERVE;
    return is_user_stack_pointer(*out_stack_pointer);
}

bool release_user_stack(void* context, uint64_t root, uint64_t cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || record->address_space.cr3 != root ||
        !record->user_stack_leased || cookie != record->user_stack_physical)
        return false;

    while (record->user_stack_mapped_pages != 0u) {
        const uint64_t page = record->user_stack_mapped_pages - 1ULL;
        const uint64_t va = USER_STACK_BASE + page * PAGE_SIZE;
        if (!starlix::mem::address_space_unmap_user_page(
                record->address_space, va)) return false;
        --record->user_stack_mapped_pages;
    }
    if (!starlix::mem::managed_release_phys_pages(
            record->user_stack_physical, USER_STACK_PAGES)) return false;
    record->user_stack_physical = 0ULL;
    record->user_stack_leased = false;
    return true;
}

bool allocate_kernel_stack(void* context, uint64_t root,
                           uint64_t* out_top, uint64_t* out_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !out_top || !out_cookie ||
        record->address_space.cr3 != root ||
        record->kernel_stack.magic != 0u) return false;
    *out_top = 0ULL;
    *out_cookie = 0ULL;
    if (!starlix::mem::kernel_stack_allocate_default(record->kernel_stack))
        return false;
    *out_top = record->kernel_stack.top;
    *out_cookie = record->kernel_stack.allocation_id;
    return true;
}

bool release_kernel_stack(void* context, uint64_t root,
                          uint64_t cookie, uint64_t thread_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || record->address_space.cr3 != root ||
        record->kernel_stack.magic != starlix::mem::KERNEL_STACK_MAGIC ||
        cookie != record->kernel_stack.allocation_id) return false;
    auto* thread = reinterpret_cast<StThread*>(
        static_cast<uintptr_t>(thread_cookie));
    if (thread_cookie != 0ULL && thread != record->thread) return false;
    if (!starlix::mem::kernel_stack_release(record->kernel_stack, thread))
        return false;
    return true;
}

bool create_process(void* context, const char* name, uint64_t root,
                    uint64_t* out_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !out_cookie || record->address_space.cr3 != root ||
        record->process) return false;
    *out_cookie = 0ULL;
    record->process = StNtCreateProcess(name, root);
    if (!record->process) return false;
    *out_cookie = reinterpret_cast<uint64_t>(record->process);
    return true;
}

bool create_thread(void* context, uint64_t process_cookie,
                   uint64_t entry, uint64_t user_stack, uint64_t kernel_top,
                   uint64_t* out_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !out_cookie || !record->process || record->thread ||
        process_cookie != reinterpret_cast<uint64_t>(record->process)) return false;
    *out_cookie = 0ULL;
    record->thread = StNtCreateThread(
        record->process, entry, user_stack, kernel_top);
    if (!record->thread) return false;
    *out_cookie = reinterpret_cast<uint64_t>(record->thread);
    return true;
}

bool bind_kernel_stack(void* context, uint64_t stack_cookie,
                       uint64_t thread_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !record->thread ||
        stack_cookie != record->kernel_stack.allocation_id ||
        thread_cookie != reinterpret_cast<uint64_t>(record->thread)) return false;
    return starlix::mem::kernel_stack_bind_thread(
        record->kernel_stack, record->thread);
}

bool abort_process(void* context, uint64_t process_cookie,
                   uint64_t thread_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !record->process ||
        process_cookie != reinterpret_cast<uint64_t>(record->process) ||
        (thread_cookie != 0ULL &&
         thread_cookie != reinterpret_cast<uint64_t>(record->thread))) return false;
    if (!process_abort_creation(record->process, record->thread)) return false;
    record->thread = nullptr;
    record->process = nullptr;
    return true;
}

bool admit_thread(void* context, uint64_t thread_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    return record && record->thread &&
        thread_cookie == reinterpret_cast<uint64_t>(record->thread) &&
        starlix::process::scheduler::scheduler_admit(record->thread);
}

bool remove_thread(void* context, uint64_t thread_cookie) {
    auto* record = static_cast<UserProcessRecord*>(context);
    if (!record || !record->thread ||
        thread_cookie != reinterpret_cast<uint64_t>(record->thread)) return false;
    /* A rejected first-entry attempt may already have removed the READY slot.
       Treat that exact absent state as clean so the caller can still abort. */
    return starlix::process::scheduler::scheduler_remove(record->thread->tid) ||
        !starlix::process::scheduler::scheduler_is_admitted(record->thread);
}

UserProcessBackend make_backend(UserProcessRecord* record) {
    UserProcessBackend backend{};
    backend.context = record;
    backend.create_address_space = create_address_space;
    backend.destroy_address_space = destroy_address_space;
    backend.map_image = map_image;
    backend.unmap_image = unmap_image;
    backend.allocate_user_stack = allocate_user_stack;
    backend.release_user_stack = release_user_stack;
    backend.allocate_kernel_stack = allocate_kernel_stack;
    backend.release_kernel_stack = release_kernel_stack;
    backend.create_process = create_process;
    backend.create_thread = create_thread;
    backend.bind_kernel_stack = bind_kernel_stack;
    backend.abort_process = abort_process;
    backend.admit_thread = admit_thread;
    backend.remove_thread = remove_thread;
    return backend;
}

void fill_handle(const UserProcessRecord& record, UserProcessHandle& out) {
    out = UserProcessHandle{};
    out.magic = USER_PROCESS_HANDLE_MAGIC;
    out.resource_cookie = record.cookie;
    out.address_space_root = record.transaction.address_space_root;
    out.entry_point = record.transaction.entry_point;
    out.user_stack_pointer = record.transaction.user_stack_pointer;
    if (record.process) out.pid = record.process->pid;
    if (record.thread) out.tid = record.thread->tid;
}

} // namespace

bool StNtCreateUserProcessFromECT(const char* image_name,
                                  const void* ect_bytes,
                                  uint64_t ect_size,
                                  UserProcessHandle* out_handle) {
    if (!out_handle) return false;
    *out_handle = UserProcessHandle{};
    if (!image_name || !ect_bytes || ect_size == 0ULL ||
        !starlix::mem::phys_allocator_ready() ||
        !starlix::mem::managed_page_pool_ready() ||
        !starlix::mem::address_space_manager_ready() ||
        !starlix::mem::kernel_stack_manager_ready() ||
        !scheduler::scheduler_ready() || context_switch::context_switch_armed() ||
        context_switch::context_switch_current() != nullptr ||
        starlix::mem::address_space_active_root() !=
            starlix::mem::address_space_kernel_root()) return false;

    const uint64_t flags = irq_save_disable();
    lock_records();
    UserProcessRecord* record = nullptr;
    for (uint32_t i = 0u; i < USER_PROCESS_RECORD_CAPACITY; ++i) {
        if (!g_records[i].used) {
            record = &g_records[i];
            break;
        }
    }
    if (!record || g_next_cookie == 0ULL || g_next_cookie == ~0ULL) {
        unlock_records();
        irq_restore(flags);
        return false;
    }
    reset_record_payload(*record);
    record->used = true;
    record->cookie = g_next_cookie++;
    unlock_records();

    const UserProcessBackend backend = make_backend(record);
    const bool created = user_process_create_transactional(
        image_name, ect_bytes, ect_size, backend, record->transaction);
    if (!created && record->transaction.magic == 0u) {
        lock_records();
        record->used = false;
        record->cookie = 0ULL;
        reset_record_payload(*record);
        unlock_records();
        irq_restore(flags);
        return false;
    }

    fill_handle(*record, *out_handle);
    irq_restore(flags);
    return created;
}

bool StNtStartUserProcess(const UserProcessHandle* handle) {
    if (!handle || handle->magic != USER_PROCESS_HANDLE_MAGIC ||
        handle->reserved != 0u || handle->resource_cookie == 0ULL) return false;

    const uint64_t flags = irq_save_disable();
    lock_records();
    UserProcessRecord* record = find_record(handle->resource_cookie);
    const uint32_t required_stages =
        USER_PROCESS_STAGE_ADDRESS_SPACE | USER_PROCESS_STAGE_IMAGE |
        USER_PROCESS_STAGE_USER_STACK | USER_PROCESS_STAGE_KERNEL_STACK |
        USER_PROCESS_STAGE_PROCESS | USER_PROCESS_STAGE_THREAD |
        USER_PROCESS_STAGE_STACK_BOUND | USER_PROCESS_STAGE_SCHEDULER;

    const bool handle_matches = record && record->used &&
        record->cookie == handle->resource_cookie && record->process &&
        record->thread && record->process->pid == handle->pid &&
        record->thread->tid == handle->tid &&
        record->transaction.address_space_root == handle->address_space_root &&
        record->transaction.entry_point == handle->entry_point &&
        record->transaction.user_stack_pointer == handle->user_stack_pointer &&
        record->process->page_directory_cr3 == handle->address_space_root;
    const bool transaction_complete = record &&
        record->transaction.magic == USER_PROCESS_TX_MAGIC &&
        (record->transaction.stages & required_stages) == required_stages &&
        record->transaction.process_cookie ==
            reinterpret_cast<uint64_t>(record->process) &&
        record->transaction.thread_cookie ==
            reinterpret_cast<uint64_t>(record->thread);
    const bool process_ready = record && record->process &&
        process_is_registered(record->process) &&
        record->process->state == PROCESS_STATE_READY;
    const bool thread_ready = record && record->thread &&
        thread_is_registered(record->thread) &&
        record->thread->parent_process == record->process &&
        record->thread->state == THREAD_STATE_READY;
    const bool scheduler_admitted = record && record->thread &&
        scheduler::scheduler_is_admitted(record->thread);
    const bool context_idle =
        !context_switch::context_switch_armed() &&
        context_switch::context_switch_current() == nullptr &&
        (!record || !record->launch_committed);
    const UserProcessStartFacts facts{
        handle_matches,
        transaction_complete,
        process_ready,
        thread_ready,
        scheduler_admitted,
        context_switch::context_switch_ready(),
        context_idle,
        starlix::mem::address_space_active_root() ==
            starlix::mem::address_space_kernel_root()
    };

    if (!user_process_start_allowed(facts)) {
        unlock_records();
        irq_restore(flags);
        return false;
    }

    StThread* thread = record->thread;
    record->launch_committed = true;
    unlock_records();

    /* The callee performs CR3/TSS/FPU/FS/GS preparation and enters via IRETQ.
       A successful transition never returns to this kernel call site. */
    const bool started = context_switch::context_switch_start_first_user(thread);
    if (!started) {
        lock_records();
        if (record->used && record->cookie == handle->resource_cookie)
            record->launch_committed = false;
        unlock_records();
        irq_restore(flags);
        return false;
    }
    return true;
}

bool StNtAbortUserProcess(UserProcessHandle* handle) {
    if (!handle || handle->magic != USER_PROCESS_HANDLE_MAGIC ||
        handle->resource_cookie == 0ULL || context_switch::context_switch_armed())
        return false;

    const uint64_t flags = irq_save_disable();
    lock_records();
    UserProcessRecord* record = find_record(handle->resource_cookie);
    unlock_records();
    if (!record || record->launch_committed || (record->thread &&
        record->thread == context_switch::context_switch_current())) {
        irq_restore(flags);
        return false;
    }

    const UserProcessBackend backend = make_backend(record);
    const bool complete = user_process_rollback_transaction(
        backend, record->transaction);
    if (complete) {
        lock_records();
        record->used = false;
        record->cookie = 0ULL;
        reset_record_payload(*record);
        unlock_records();
        *handle = UserProcessHandle{};
    } else {
        fill_handle(*record, *handle);
    }
    irq_restore(flags);
    return complete;
}

StThread* user_process_main_thread(const UserProcessHandle* handle) {
    if (!handle || handle->magic != USER_PROCESS_HANDLE_MAGIC ||
        handle->resource_cookie == 0ULL) return nullptr;
    lock_records();
    UserProcessRecord* record = find_record(handle->resource_cookie);
    StThread* result = record &&
        (record->transaction.stages & USER_PROCESS_STAGE_THREAD) != 0u
        ? record->thread : nullptr;
    unlock_records();
    return result;
}

} // namespace starlix::process
