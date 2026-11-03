#include "../kernel/src/process/user_process_transaction.hpp"

#include <cstdio>
#include <cstring>

using namespace starlix::process;

struct Mock {
    int fail_step = 0;
    int fail_cleanup_event = 0;
    bool cleanup_failed_once = false;
    bool address = false;
    bool image = false;
    bool user_stack = false;
    bool kernel_stack = false;
    bool process = false;
    bool thread = false;
    bool admitted = false;
    int events[64]{};
    int event_count = 0;
};

static void event(Mock& m, int id) {
    if (m.event_count < 64) m.events[m.event_count++] = id;
}
static bool step(Mock& m, int id) { return m.fail_step != id; }
static bool cleanup(Mock& m, int id) {
    event(m, id);
    if (m.fail_cleanup_event == id && !m.cleanup_failed_once) {
        m.cleanup_failed_once = true;
        return false;
    }
    return true;
}

static bool create_as(void* c, StlUserU64* root) {
    auto& m = *static_cast<Mock*>(c); event(m, 1); m.address = true;
    *root = 0x1000; return step(m, 1);
}
static bool destroy_as(void* c, StlUserU64 root) {
    auto& m = *static_cast<Mock*>(c); if (root != 0x1000) return false;
    if (!cleanup(m, 14)) return false;
    m.address = false;
    return true;
}
static bool map_img(void* c, const void* bytes, StlUserU64 size,
                    StlUserU64 root, StlUserU64* entry, StlUserU64* cookie) {
    auto& m = *static_cast<Mock*>(c); event(m, 2);
    if (!bytes || !size || root != 0x1000) return false;
    m.image = true; *entry = 0x8000101000ULL; *cookie = 0x22;
    return step(m, 2);
}
static bool unmap_img(void* c, StlUserU64 root, StlUserU64 cookie) {
    auto& m = *static_cast<Mock*>(c);
    if (root != 0x1000 || cookie != 0x22 || !cleanup(m, 13)) return false;
    m.image = false; return true;
}
static bool alloc_user(void* c, StlUserU64 root, StlUserU64* sp,
                       StlUserU64* cookie) {
    auto& m = *static_cast<Mock*>(c); event(m, 3);
    if (root != 0x1000) return false;
    m.user_stack = true; *sp = 0xFFFFEFFFF0ULL; *cookie = 0x33;
    return step(m, 3);
}
static bool release_user(void* c, StlUserU64 root, StlUserU64 cookie) {
    auto& m = *static_cast<Mock*>(c);
    if (root != 0x1000 || cookie != 0x33 || !cleanup(m, 12)) return false;
    m.user_stack = false; return true;
}
static bool alloc_kernel(void* c, StlUserU64 root, StlUserU64* top,
                         StlUserU64* cookie) {
    auto& m = *static_cast<Mock*>(c); event(m, 4);
    if (root != 0x1000) return false;
    m.kernel_stack = true; *top = 0xFFFFFF0000005000ULL; *cookie = 0x44;
    return step(m, 4);
}
static bool release_kernel(void* c, StlUserU64 root, StlUserU64 cookie,
                           StlUserU64 thread) {
    auto& m = *static_cast<Mock*>(c);
    if (root != 0x1000 || cookie != 0x44 ||
        (thread != 0x66 && thread != 0x0ULL) || !cleanup(m, 10)) return false;
    m.kernel_stack = false; return true;
}
static bool create_proc(void* c, const char*, StlUserU64 root,
                        StlUserU64* cookie) {
    auto& m = *static_cast<Mock*>(c); event(m, 5);
    if (root != 0x1000) return false;
    m.process = true; *cookie = 0x55; return step(m, 5);
}
static bool create_thr(void* c, StlUserU64 proc, StlUserU64 entry,
                       StlUserU64 usp, StlUserU64 ksp, StlUserU64* cookie) {
    auto& m = *static_cast<Mock*>(c); event(m, 6);
    if (proc != 0x55 || entry == 0 || usp == 0 || ksp == 0) return false;
    m.thread = true; *cookie = 0x66; return step(m, 6);
}
static bool bind_stack(void* c, StlUserU64 stack, StlUserU64 thread) {
    auto& m = *static_cast<Mock*>(c); event(m, 7);
    return stack == 0x44 && thread == 0x66 && step(m, 7);
}
static bool abort_proc(void* c, StlUserU64 proc, StlUserU64 thread) {
    auto& m = *static_cast<Mock*>(c); if (proc != 0x55 ||
        (thread != 0x66 && thread != 0ULL) || !cleanup(m, 11)) return false;
    m.thread = false; m.process = false; return true;
}
static bool admit(void* c, StlUserU64 thread) {
    auto& m = *static_cast<Mock*>(c); event(m, 8);
    if (thread != 0x66) return false;
    if (!step(m, 8)) return false;
    m.admitted = true; return true;
}
static bool remove_thread(void* c, StlUserU64 thread) {
    auto& m = *static_cast<Mock*>(c);
    if (thread != 0x66 || !cleanup(m, 9)) return false;
    m.admitted = false; return true;
}

static UserProcessBackend backend_for(Mock& m) {
    UserProcessBackend b{}; b.context = &m;
    b.create_address_space = create_as; b.destroy_address_space = destroy_as;
    b.map_image = map_img; b.unmap_image = unmap_img;
    b.allocate_user_stack = alloc_user; b.release_user_stack = release_user;
    b.allocate_kernel_stack = alloc_kernel; b.release_kernel_stack = release_kernel;
    b.create_process = create_proc; b.create_thread = create_thr;
    b.bind_kernel_stack = bind_stack; b.abort_process = abort_proc;
    b.admit_thread = admit; b.remove_thread = remove_thread;
    return b;
}

static bool all_clear(const Mock& m) {
    return !m.address && !m.image && !m.user_stack && !m.kernel_stack &&
           !m.process && !m.thread && !m.admitted;
}
static bool has_event(const Mock& m, int id) {
    for (int i = 0; i < m.event_count; ++i) if (m.events[i] == id) return true;
    return false;
}

int main() {
    int failures = 0;
    const unsigned char image[4] = {1,2,3,4};
    {
        Mock m; auto b = backend_for(m); UserProcessTransaction tx{};
        if (!user_process_create_transactional("stlcs", image, sizeof(image), b, tx) ||
            tx.magic != USER_PROCESS_TX_MAGIC || tx.stages != 0xFFu ||
            !m.address || !m.image || !m.user_stack || !m.kernel_stack ||
            !m.process || !m.thread || !m.admitted) ++failures;
        if (!user_process_rollback_transaction(b, tx) || tx.magic != 0u ||
            !all_clear(m) || !has_event(m, 9) || !has_event(m, 10) ||
            !has_event(m, 11) || !has_event(m, 12) || !has_event(m, 13) ||
            !has_event(m, 14)) { std::printf("success/rollback test failed: magic=%x stages=%x clear=%d\n", tx.magic, tx.stages, all_clear(m)); ++failures; }
    }
    for (int fail = 1; fail <= 8; ++fail) {
        Mock m; m.fail_step = fail; auto b = backend_for(m);
        UserProcessTransaction tx{};
        if (user_process_create_transactional("stlcs", image, sizeof(image), b, tx))
            ++failures;
        if (tx.magic != 0u || !all_clear(m)) { std::printf("failcase %d residual: magic=%x stages=%x clear=%d\n", fail, tx.magic, tx.stages, all_clear(m)); ++failures; }
        if (fail < 8 && m.admitted) { std::printf("failcase %d admitted\n", fail); ++failures; }
    }
    {
        Mock m; m.fail_step = 8; m.fail_cleanup_event = 10;
        auto b = backend_for(m); UserProcessTransaction tx{};
        if (user_process_create_transactional("stlcs", image, sizeof(image), b, tx) ||
            tx.magic != USER_PROCESS_TX_MAGIC || !m.kernel_stack ||
            !m.process || !m.thread || !m.address || !m.image || !m.user_stack)
            ++failures;
        m.fail_cleanup_event = 0;
        if (!user_process_rollback_transaction(b, tx) || tx.magic != 0u ||
            !all_clear(m)) { std::printf("retry rollback failed: magic=%x stages=%x clear=%d\n", tx.magic, tx.stages, all_clear(m)); ++failures; }
    }
    {
        Mock m; auto b = backend_for(m); UserProcessTransaction tx{};
        if (user_process_create_transactional("", image, sizeof(image), b, tx) ||
            tx.magic != 0u || m.event_count != 0) ++failures;
    }
    {
        Mock m; auto b = backend_for(m); b.remove_thread = nullptr;
        UserProcessTransaction tx{};
        if (user_process_create_transactional("stlcs", image, sizeof(image), b, tx) ||
            tx.magic != 0u || m.event_count != 0) ++failures;
    }
    std::printf("User-process Phase 2L transaction self-test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
