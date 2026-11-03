#include <cstdio>
#include <cstring>
#include "freestanding_host_types.hpp"
#include "../kernel/src/mem/managed_page_pool.hpp"

namespace {
constexpr uint64_t PAGE = 4096ULL;
constexpr uint32_t CAPACITY = 64u;
alignas(4096) uint8_t physical_memory[CAPACITY * PAGE]{};
uint32_t next_page = 1u;

bool fake_ready() { return true; }
uint64_t fake_allocate(uint64_t count) {
    if (count == 0u || count > CAPACITY || count > CAPACITY - next_page)
        return 0ULL;
    const uint64_t address = reinterpret_cast<uintptr_t>(physical_memory +
        static_cast<uint64_t>(next_page) * PAGE);
    next_page += static_cast<uint32_t>(count);
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
                0, static_cast<size_t>(count * PAGE));
    return address;
}
unsigned passed = 0u;
unsigned failed = 0u;
void check(bool condition, const char* name) {
    if (condition) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", name); }
}
}

namespace starlix::mem {
bool phys_allocator_ready() { return fake_ready(); }
uint64_t alloc_phys_pages(uint64_t count) { return fake_allocate(count); }
}

int main() {
    using namespace starlix::mem;
    check(!managed_page_pool_ready() && managed_alloc_phys_page() == 0ULL,
          "refuses managed allocations before explicit pool initialization");
    check(managed_page_pool_init() && managed_page_pool_ready(),
          "initializes the managed pool after physical allocator readiness");
    check(managed_page_pool_init(), "managed pool initialization is idempotent");
    const uint64_t run = managed_alloc_phys_pages(3u);
    check(run != 0ULL && (run & (PAGE - 1ULL)) == 0ULL,
          "allocates a contiguous managed page run");
    if (run) {
        auto* bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(run));
        bytes[0] = 0xA5u;
        bytes[PAGE] = 0x5Au;
        check(!managed_release_phys_pages(run, 2u),
              "rejects a partial release of a multi-page lease");
        check(managed_release_phys_pages(run, 3u),
              "releases an exact managed page run");
        check(!managed_release_phys_pages(run, 3u),
              "rejects a double release");
        const uint64_t reused_run = managed_alloc_phys_pages(3u);
        check(reused_run == run,
              "coalesced free extent satisfies a contiguous multi-page request");
        auto* reused_bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(reused_run));
        check(reused_bytes[0] == 0u && reused_bytes[PAGE] == 0u &&
              reused_bytes[3u * PAGE - 1u] == 0u,
              "reused contiguous run is fully zeroed before publication");
        check(managed_release_phys_pages(reused_run, 3u),
              "releases reused contiguous run");
        const uint64_t reused = managed_alloc_phys_page();
        check(reused == run,
              "single-page allocations reuse the first page of a free extent");
        check(managed_release_phys_page(reused),
              "releases reused single page and coalesces adjacent free pages");
        check(!managed_release_phys_page(run + PAGE + 1u),
              "rejects unaligned release addresses");
    } else {
        check(false, "contiguous allocation exists for release checks");
        check(false, "double release rejected");
        check(false, "coalesced run reused");
        check(false, "reused run zeroed");
        check(false, "reused run released");
        check(false, "reused single page");
        check(false, "single page released");
        check(false, "unaligned release rejected");
    }
    check(managed_alloc_phys_pages(0u) == 0ULL,
          "rejects zero-page allocation");
    std::printf("Managed page pool Phase 2K self-test: %u passed, %u failed\n",
                passed, failed);
    return failed == 0u ? 0 : 1;
}
