#include <cstdio>
#include "freestanding_host_types.hpp"
#include "../kernel/src/mem/address_space.hpp"

using namespace starlix::mem;
using namespace starlix::mem::user_address;

namespace {
constexpr uint64_t FAKE_PHYS_BASE = 0x10000000ULL;
constexpr uint32_t FAKE_PAGE_COUNT = 128u;
alignas(4096) uint64_t fake_pages[FAKE_PAGE_COUNT][512]{};
uint32_t next_fake_page = 1u;
uint32_t tests = 0u;
uint32_t failures = 0u;

void check(bool condition, const char* description) {
    ++tests;
    if (!condition) {
        ++failures;
        printf("[FAIL] %s\n", description);
    } else {
        printf("[PASS] %s\n", description);
    }
}

uint64_t fake_allocate_page() {
    if (next_fake_page >= FAKE_PAGE_COUNT) return 0ULL;
    const uint32_t index = next_fake_page++;
    for (uint32_t i = 0; i < 512u; ++i) fake_pages[index][i] = 0ULL;
    return FAKE_PHYS_BASE + static_cast<uint64_t>(index) * 4096ULL;
}

volatile uint64_t* fake_map_table(uint64_t physical) {
    if (physical < FAKE_PHYS_BASE ||
        (physical & 4095ULL) != 0ULL) return nullptr;
    const uint64_t offset = physical - FAKE_PHYS_BASE;
    const uint64_t index = offset / 4096ULL;
    if (index >= FAKE_PAGE_COUNT) return nullptr;
    return fake_pages[index];
}

uint64_t fake_critical_enter() { return 0x202ULL; }
void fake_critical_leave(uint64_t) {}
bool fake_switch_root(uint64_t, uint64_t) { return true; }

void fake_invalidate_page(uint64_t) {}

AddressSpacePlatform valid_platform() {
    AddressSpacePlatform p{};
    p.cr0 = 1ULL | (1ULL << 31) | (1ULL << 16); // PE | PG | WP
    p.cr3 = FAKE_PHYS_BASE;
    p.cr4 = (1ULL << 5); // PAE; LA57 clear
    p.efer = (1ULL << 10) | (1ULL << 11); // LMA | NXE
    p.physical_address_bits = 36u;
    p.allocate_page = fake_allocate_page;
    p.map_table = fake_map_table;
    p.critical_enter = fake_critical_enter;
    p.critical_leave = fake_critical_leave;
    p.switch_root = fake_switch_root;
    p.invalidate_page = fake_invalidate_page;
    p.online_cpu_count = 1u;
    return p;
}

void reset_fake_memory() {
    for (uint32_t p = 0; p < FAKE_PAGE_COUNT; ++p)
        for (uint32_t i = 0; i < 512u; ++i) fake_pages[p][i] = 0ULL;
    next_fake_page = 1u;
    fake_pages[0][0] = 0x3007ULL; // Present, writable, user: clone must scrub U/S.
    fake_pages[0][256] = 0x5007ULL;
}

volatile uint64_t* fake_leaf_table(const AddressSpace& space, uint64_t va) {
    volatile uint64_t* pml4 = fake_map_table(space.cr3);
    if (!pml4) return nullptr;
    uint64_t entry = pml4[(va >> 39) & 0x1FFULL];
    volatile uint64_t* pdpt = fake_map_table(entry & PTE_ADDRESS_MASK);
    if (!pdpt) return nullptr;
    entry = pdpt[(va >> 30) & 0x1FFULL];
    volatile uint64_t* pd = fake_map_table(entry & PTE_ADDRESS_MASK);
    if (!pd) return nullptr;
    entry = pd[(va >> 21) & 0x1FFULL];
    volatile uint64_t* pt = fake_map_table(entry & PTE_ADDRESS_MASK);
    return pt;
}

bool init_manager() {
    return address_space_manager_init(valid_platform());
}
}

int main() {
    reset_fake_memory();
    check(init_manager(), "manager accepts valid four-level paging and NX platform");
    check(address_space_manager_ready(), "manager publishes ready state after validation");
    check(address_space_kernel_root() == FAKE_PHYS_BASE,
          "manager records current kernel PML4 physical address");

    AddressSpace first{};
    check(address_space_create(first), "new process address space clones kernel PML4");
    check(first.cr3 != FAKE_PHYS_BASE && (first.cr3 & 4095ULL) == 0ULL,
          "new address space has a distinct aligned PML4 page");
    check(first.magic == ADDRESS_SPACE_MAGIC && first.mapped_pages == 0u,
          "created address space has valid metadata and no mapped pages");

    volatile uint64_t* first_root = fake_map_table(first.cr3);
    check(first_root && (first_root[0] & 0x7ULL) == 0x3ULL,
          "cloned low kernel mapping is present but supervisor-only");
    check(first_root && (first_root[256] & 0x7ULL) == 0x3ULL,
          "cloned high kernel mapping is also supervisor-only");
    check((fake_pages[0][0] & 0x7ULL) == 0x7ULL,
          "source kernel PML4 remains unchanged by clone scrubbing");
    check(first_root && first_root[1] == 0ULL,
          "private user PML4 slot starts empty");

    check(address_space_map_user_page(first, USER_IMAGE_BASE, 0x40000000ULL,
              USER_PAGE_READ | USER_PAGE_EXEC),
          "maps a read/execute user image page");
    check(first.mapped_pages == 1u, "mapped page counter increments");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE, 0x40001000ULL,
              USER_PAGE_READ | USER_PAGE_EXEC),
          "duplicate virtual mapping is rejected");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 1ULL, 0x40002000ULL,
              USER_PAGE_READ),
          "unaligned virtual mapping is rejected");
    check(!address_space_map_user_page(first, USER_REGION_END, 0x40002000ULL,
              USER_PAGE_READ),
          "mapping outside the reserved user region is rejected");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL, 0x40002001ULL,
              USER_PAGE_READ),
          "unaligned physical frame is rejected");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL, 0x1000000000ULL,
              USER_PAGE_READ),
          "physical frame above the reported CPU address width is rejected");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL, 0ULL,
              USER_PAGE_READ),
          "physical frame zero is rejected");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL, 0x40002000ULL,
              USER_PAGE_READ | USER_PAGE_WRITE | USER_PAGE_EXEC),
          "writable and executable mapping is rejected by W^X");
    check(!address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL, 0x40002000ULL,
              USER_PAGE_WRITE),
          "mapping without read permission is rejected");

    uint64_t translated = 0ULL;
    uint32_t permissions = 0u;
    check(address_space_translate_user(first, USER_IMAGE_BASE + 0x123ULL,
              translated, permissions) && translated == 0x40000123ULL,
          "user translation preserves the in-page offset");
    check(permissions == (USER_PAGE_READ | USER_PAGE_EXEC),
          "translation reports read/execute permissions accurately");
    volatile uint64_t* first_leaf = fake_leaf_table(first, USER_IMAGE_BASE);
    const uint64_t saved_exec_leaf = first_leaf ? first_leaf[(USER_IMAGE_BASE >> 12) & 0x1FFULL] : 0ULL;
    if (first_leaf) first_leaf[(USER_IMAGE_BASE >> 12) & 0x1FFULL] =
        (saved_exec_leaf | (1ULL << 1)) & ~PTE_NX;
    check(!address_space_translate_user(first, USER_IMAGE_BASE,
              translated, permissions),
          "corrupted writable/executable leaf entry is rejected");
    if (first_leaf) first_leaf[(USER_IMAGE_BASE >> 12) & 0x1FFULL] = saved_exec_leaf;

    check(address_space_map_user_page(first, USER_IMAGE_BASE + 4096ULL,
              0x40002000ULL, USER_PAGE_READ | USER_PAGE_WRITE),
          "maps a separate read/write non-executable user page");
    check(address_space_translate_user(first, USER_IMAGE_BASE + 4096ULL,
              translated, permissions) && translated == 0x40002000ULL &&
              permissions == (USER_PAGE_READ | USER_PAGE_WRITE),
          "read/write mapping translates with NX enforced");

    check(address_space_map_user_page(first, USER_STACK_BASE, 0x40003000ULL,
              USER_PAGE_READ | USER_PAGE_WRITE),
          "maps a page in the planned user stack region");
    check(address_space_unmap_user_page(first, USER_IMAGE_BASE + 4096ULL),
          "unmaps a present user page");
    check(first.mapped_pages == 2u,
          "unmap decrements mapping count without reclaiming shared frames");
    check(!address_space_translate_user(first, USER_IMAGE_BASE + 4096ULL,
              translated, permissions),
          "unmapped user page no longer translates");
    check(!address_space_unmap_user_page(first, USER_IMAGE_BASE + 4096ULL),
          "double unmap is rejected");

    AddressSpace second{};
    check(address_space_create(second), "second process receives a separate page-table root");
    check(address_space_map_user_page(second, USER_IMAGE_BASE, 0x50000000ULL,
              USER_PAGE_READ | USER_PAGE_EXEC),
          "second process maps the same virtual address independently");
    check(address_space_translate_user(second, USER_IMAGE_BASE, translated, permissions) &&
              translated == 0x50000000ULL,
          "second process sees its own physical frame");
    check(address_space_translate_user(first, USER_IMAGE_BASE, translated, permissions) &&
              translated == 0x40000000ULL,
          "first process mapping remains isolated from second process");
    first_root = fake_map_table(first.cr3);
    if (first_root) first_root[1] |= PTE_NX;
    check(!address_space_translate_user(first, USER_IMAGE_BASE, translated, permissions),
          "corrupted NX parent entry is rejected by the page-table walker");
    if (first_root) first_root[1] &= ~PTE_NX;

    AddressSpace forged{0x60000000ULL, 0u, ADDRESS_SPACE_MAGIC};
    check(!address_space_map_user_page(forged, USER_IMAGE_BASE, 0x40004000ULL,
              USER_PAGE_READ),
          "unregistered/forged address-space object is rejected");

    reset_fake_memory();
    AddressSpacePlatform bad = valid_platform();
    fake_pages[0][1] = 0x7003ULL;
    check(!address_space_manager_init(bad),
          "manager refuses to overwrite an occupied reserved PML4 slot");

    reset_fake_memory();
    bad = valid_platform(); bad.cr4 |= (1ULL << 12);
    check(!address_space_manager_init(bad), "five-level paging is rejected fail-closed");
    reset_fake_memory();
    bad = valid_platform(); bad.efer &= ~(1ULL << 11);
    check(!address_space_manager_init(bad), "NX-disabled CPU state is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.cr0 &= ~(1ULL << 16);
    check(!address_space_manager_init(bad), "CR0.WP-disabled CPU state is rejected");
    bad = valid_platform(); bad.cr0 &= ~1ULL;
    check(!address_space_manager_init(bad), "CR0.PE-disabled CPU state is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.cr0 &= ~(1ULL << 31);
    check(!address_space_manager_init(bad), "paging-disabled CPU state is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.cr4 &= ~(1ULL << 5);
    check(!address_space_manager_init(bad), "PAE-disabled CPU state is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.physical_address_bits = 53u;
    check(!address_space_manager_init(bad), "unsupported physical address width is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.cr3 = 0ULL;
    check(!address_space_manager_init(bad), "null kernel page-table root is rejected");
    reset_fake_memory();
    bad = valid_platform(); bad.cr3 |= (1ULL << 60);
    check(!address_space_manager_init(bad), "reserved CR3 high bits are rejected");

    reset_fake_memory();
    check(init_manager(), "manager can initialize again after fail-closed probes");
    AddressSpace many[ADDRESS_SPACE_MAX]{};
    bool all_created = true;
    for (uint32_t i = 0; i < ADDRESS_SPACE_MAX; ++i)
        if (!address_space_create(many[i])) all_created = false;
    check(all_created, "bounded address-space registry reaches its configured capacity");
    AddressSpace overflow{};
    check(!address_space_create(overflow), "address-space registry rejects overflow");

    printf("ADDRESS_SPACE_PHASE2B_SELFTEST: %s (%u checks, %u failures)\n",
           failures == 0u ? "PASS" : "FAIL", tests, failures);
    return failures == 0u ? 0 : 1;
}
