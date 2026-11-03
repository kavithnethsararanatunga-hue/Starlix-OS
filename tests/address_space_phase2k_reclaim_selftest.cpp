#include <cstdio>
#include "freestanding_host_types.hpp"
#include "../kernel/src/mem/address_space.hpp"

using namespace starlix::mem;
using namespace starlix::mem::user_address;

namespace {
constexpr uint64_t BASE = 0x10000000ULL;
constexpr uint32_t PAGE_COUNT = 128u;
alignas(4096) uint64_t pages[PAGE_COUNT][512]{};
bool allocated[PAGE_COUNT]{};
uint32_t alloc_calls = 0u;
uint32_t release_calls = 0u;
uint32_t fail_alloc_at = 0u;
uint32_t passed = 0u;
uint32_t failed = 0u;
uint64_t current_root = BASE;

void check(bool condition, const char* name) {
    if (condition) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", name); }
}

uint64_t allocate_page() {
    ++alloc_calls;
    if (fail_alloc_at == alloc_calls) return 0ULL;
    for (uint32_t i = 1; i < PAGE_COUNT; ++i) {
        if (!allocated[i]) {
            allocated[i] = true;
            for (uint32_t j = 0; j < 512u; ++j) pages[i][j] = 0ULL;
            return BASE + static_cast<uint64_t>(i) * PAGE_SIZE;
        }
    }
    return 0ULL;
}

bool release_page(uint64_t physical) {
    ++release_calls;
    if (physical < BASE || ((physical - BASE) & (PAGE_SIZE - 1ULL)) != 0ULL)
        return false;
    const uint64_t index = (physical - BASE) / PAGE_SIZE;
    if (index == 0ULL || index >= PAGE_COUNT || !allocated[index]) return false;
    allocated[index] = false;
    return true;
}

volatile uint64_t* map_table(uint64_t physical) {
    if (physical < BASE || ((physical - BASE) & (PAGE_SIZE - 1ULL)) != 0ULL)
        return nullptr;
    const uint64_t index = (physical - BASE) / PAGE_SIZE;
    if (index >= PAGE_COUNT || !allocated[index]) return nullptr;
    return pages[index];
}

uint64_t critical_enter() { return 0x202ULL; }
void critical_leave(uint64_t) {}
bool switch_root(uint64_t expected, uint64_t target) {
    if (expected != current_root || target == 0ULL) return false;
    current_root = target;
    return true;
}
void invalidate_page(uint64_t) {}

AddressSpacePlatform platform() {
    AddressSpacePlatform p{};
    p.cr0 = 1ULL | (1ULL << 31) | (1ULL << 16);
    p.cr3 = BASE;
    p.cr4 = 1ULL << 5;
    p.efer = (1ULL << 10) | (1ULL << 11);
    p.physical_address_bits = 36u;
    p.allocate_page = allocate_page;
    p.release_page = release_page;
    p.map_table = map_table;
    p.critical_enter = critical_enter;
    p.critical_leave = critical_leave;
    p.switch_root = switch_root;
    p.invalidate_page = invalidate_page;
    p.online_cpu_count = 1u;
    return p;
}

uint32_t allocated_count() {
    uint32_t count = 0u;
    for (uint32_t i = 1u; i < PAGE_COUNT; ++i) if (allocated[i]) ++count;
    return count;
}

void reset() {
    for (uint32_t i = 0; i < PAGE_COUNT; ++i) {
        allocated[i] = false;
        for (uint32_t j = 0; j < 512u; ++j) pages[i][j] = 0ULL;
    }
    allocated[0] = true; // Firmware-owned kernel PML4.
    pages[0][0] = 0x3007ULL;
    pages[0][256] = 0x5007ULL;
    alloc_calls = release_calls = fail_alloc_at = 0u;
    current_root = BASE;
}
}

int main() {
    reset();
    check(address_space_manager_init(platform()), "manager initializes with reclaim callback");
    AddressSpace space{};
    check(address_space_create(space), "creates isolated test address space");
    check(address_space_map_user_page(space, USER_IMAGE_BASE, 0x40000000ULL,
              USER_PAGE_READ | USER_PAGE_EXEC), "maps one user image page");
    const uint32_t allocations_after_map = allocated_count();
    check(allocations_after_map == 6u,
          "one mapping owns root, module/stack arenas and three user page-table pages");
    check(address_space_unmap_user_page(space, USER_IMAGE_BASE),
          "unmaps final user page");
    check(allocated_count() == 3u && release_calls == 3u,
          "empty PT, PD and PDPT pages are reclaimed while module/stack arenas remain");
    volatile uint64_t* root = map_table(space.cr3);
    check(root && root[1] == 0ULL,
          "empty private user PML4 slot is cleared after reclaim");

    // Fail while constructing the second child table: the newly-created empty
    // PDPT must be detached and returned, leaving the address space unchanged.
    const uint32_t before_alloc = alloc_calls;
    const uint32_t before_release = release_calls;
    fail_alloc_at = before_alloc + 2u;
    check(!address_space_map_user_page(space, USER_IMAGE_BASE + 0x40000000ULL,
              0x40001000ULL, USER_PAGE_READ | USER_PAGE_EXEC),
          "injected intermediate page-table allocation failure is rejected");
    check(space.mapped_pages == 0u && root && root[1] == 0ULL,
          "failed map leaves no user mapping or partial parent table");
    check(release_calls == before_release + 1u && allocated_count() == 3u,
          "failed map reclaims the partially-created empty PDPT");

    const uint64_t test_root = space.cr3;
    check(address_space_activate(space),
          "test can activate the clone to exercise destroy safety");
    check(!address_space_destroy(space) && space.cr3 == test_root,
          "active address space cannot be destroyed");
    check(address_space_activate_kernel(),
          "test returns to the kernel address space before destruction");
    const uint32_t before_destroy_release = release_calls;
    check(address_space_destroy(space),
          "inactive empty address space can be destroyed");
    check(space.cr3 == 0ULL && space.magic == 0u &&
          !address_space_root_registered(test_root),
          "destroy clears the caller handle and unregisters its CR3");
    check(allocated_count() == 2u &&
          release_calls == before_destroy_release + 1u,
          "destroy releases the private PML4 while preserving module/stack arena tables");

    std::printf("Address-space Phase 2K reclaim self-test: %u passed, %u failed\n",
                passed, failed);
    return failed == 0u ? 0 : 1;
}
