#include <cstdio>
#include <cstring>
#include "freestanding_host_types.hpp"
#include "../kernel/src/mem/kernel_stack.hpp"
#include "../kernel/src/mem/address_space.hpp"
#include "../kernel/src/process/process.hpp"

using namespace starlix::mem;
using namespace starlix::mem::user_address;
using namespace starlix::process;

namespace {
constexpr uint64_t FAKE_PHYS_BASE = 0x10000000ULL;
constexpr uint32_t FAKE_PAGE_COUNT = 2048u;
alignas(4096) uint64_t fake_pages[FAKE_PAGE_COUNT][512]{};
uint32_t next_fake_page = 1u;
uint32_t checks = 0u;
uint32_t failures = 0u;
uint64_t fake_irq_flags = 0x202ULL;
uint32_t fake_invalidation_count = 0u;
uint32_t fake_cache_flush_count = 0u;
uint32_t managed_release_failures_remaining = 0u;
uint64_t fake_identity_pdpt_physical = 0ULL;
uint64_t fake_identity_pd_physical = 0ULL;
uint64_t fake_identity_pt_physical[4]{};
StThread* registered_threads[4]{};
uint32_t registered_thread_count = 0u;
StProcess* registered_processes[4]{};
uint32_t registered_process_count = 0u;

void check(bool condition, const char* description) {
    ++checks;
    if (condition) std::printf("[PASS] %s\n", description);
    else { ++failures; std::printf("[FAIL] %s\n", description); }
}

uint64_t reserve_fake_pages(uint32_t count) {
    if (count == 0u || count > FAKE_PAGE_COUNT - next_fake_page) return 0ULL;
    const uint32_t first = next_fake_page;
    next_fake_page += count;
    for (uint32_t p = first; p < next_fake_page; ++p)
        for (uint32_t i = 0; i < 512u; ++i) fake_pages[p][i] = 0ULL;
    return FAKE_PHYS_BASE + static_cast<uint64_t>(first) * PAGE_SIZE;
}

uint64_t fake_allocate_page() { return reserve_fake_pages(1u); }
volatile uint64_t* fake_map_table(uint64_t physical) {
    if (physical < FAKE_PHYS_BASE || (physical & (PAGE_SIZE - 1ULL)) != 0ULL)
        return nullptr;
    const uint64_t index = (physical - FAKE_PHYS_BASE) / PAGE_SIZE;
    return index < FAKE_PAGE_COUNT ? fake_pages[index] : nullptr;
}
uint64_t fake_critical_enter() {
    const uint64_t saved = fake_irq_flags;
    fake_irq_flags &= ~(1ULL << 9);
    return saved;
}
void fake_critical_leave(uint64_t flags) {
    fake_irq_flags = (fake_irq_flags & ~(1ULL << 9)) | (flags & (1ULL << 9));
}
bool fake_switch_root(uint64_t, uint64_t) { return false; }

void fake_invalidate_page(uint64_t) { ++fake_invalidation_count; }
void fake_flush_caches() { ++fake_cache_flush_count; }

volatile uint64_t* fake_stack_leaf(uint64_t root_physical, uint64_t va) {
    volatile uint64_t* root = fake_map_table(root_physical);
    if (!root) return nullptr;
    uint64_t entry = root[KERNEL_STACK_PML4_INDEX];
    volatile uint64_t* pdpt = fake_map_table(entry & PTE_ADDRESS_MASK);
    if (!pdpt) return nullptr;
    entry = pdpt[(va >> 30) & 0x1FFULL];
    volatile uint64_t* pd = fake_map_table(entry & PTE_ADDRESS_MASK);
    if (!pd) return nullptr;
    entry = pd[(va >> 21) & 0x1FFULL];
    volatile uint64_t* pt = fake_map_table(entry & PTE_ADDRESS_MASK);
    if (!pt) return nullptr;
    return &pt[(va >> 12) & 0x1FFULL];
}

void fake_identity_map_page(uint64_t physical, uint64_t leaf_flags) {
    if (fake_identity_pdpt_physical == 0ULL) {
        fake_identity_pdpt_physical = reserve_fake_pages(1u);
        fake_identity_pd_physical = reserve_fake_pages(1u);
        fake_pages[0][0] = fake_identity_pdpt_physical | 0x3ULL;
        auto* pdpt = fake_map_table(fake_identity_pdpt_physical);
        auto* pd = fake_map_table(fake_identity_pd_physical);
        if (!pdpt || !pd) return;
        pdpt[0] = fake_identity_pd_physical | 0x3ULL;
    }
    if (physical < FAKE_PHYS_BASE ||
        physical >= FAKE_PHYS_BASE + (8ULL * 1024ULL * 1024ULL)) return;
    const uint32_t pd_index = static_cast<uint32_t>((physical >> 21) & 0x1FFULL);
    if (pd_index < 128u || pd_index > 131u) return;
    const uint32_t pt_slot = pd_index - 128u;
    if (fake_identity_pt_physical[pt_slot] == 0ULL) {
        fake_identity_pt_physical[pt_slot] = reserve_fake_pages(1u);
        auto* pd = fake_map_table(fake_identity_pd_physical);
        if (!pd) return;
        pd[pd_index] = fake_identity_pt_physical[pt_slot] | 0x3ULL;
    }
    auto* pt = fake_map_table(fake_identity_pt_physical[pt_slot]);
    if (!pt) return;
    pt[(physical >> 12) & 0x1FFULL] = physical | 1ULL | leaf_flags;
}

AddressSpacePlatform valid_address_platform() {
    AddressSpacePlatform p{};
    p.cr0 = 1ULL | (1ULL << 31) | (1ULL << 16);
    p.cr3 = FAKE_PHYS_BASE;
    p.cr4 = (1ULL << 5);
    p.efer = (1ULL << 10) | (1ULL << 11);
    p.physical_address_bits = 36u;
    p.allocate_page = fake_allocate_page;
    p.map_table = fake_map_table;
    p.critical_enter = fake_critical_enter;
    p.critical_leave = fake_critical_leave;
    p.switch_root = fake_switch_root;
    p.invalidate_page = fake_invalidate_page;
    p.flush_caches = fake_flush_caches;
    p.online_cpu_count = 1u;
    return p;
}

void reset_fake() {
    std::memset(fake_pages, 0, sizeof(fake_pages));
    next_fake_page = 1u;

    /* Build the shared supervisor identity subtree before process roots
       are cloned. MMIO tests may populate lower identity tables later,
       but must never replace PML4 slot zero after cloning. */
    fake_identity_pdpt_physical = reserve_fake_pages(1u);
    fake_identity_pd_physical = reserve_fake_pages(1u);

    auto* initial_identity_pdpt =
        fake_map_table(fake_identity_pdpt_physical);
    if (initial_identity_pdpt &&
        fake_identity_pd_physical != 0ULL) {
        initial_identity_pdpt[0] =
            fake_identity_pd_physical | 0x3ULL;
        fake_pages[0][0] =
            fake_identity_pdpt_physical | 0x3ULL;
    }

    fake_irq_flags = 0x202ULL;
    fake_invalidation_count = 0u;
    fake_cache_flush_count = 0u;
    managed_release_failures_remaining = 0u;
    for (uint32_t i = 0; i < 4u; ++i) fake_identity_pt_physical[i] = 0ULL;
    registered_thread_count = 0u;
    registered_process_count = 0u;
}

} // namespace

namespace starlix::mem {
bool phys_allocator_ready() { return true; }
bool managed_page_pool_ready() { return true; }
uint64_t alloc_phys_pages(uint64_t page_count) {
    if (page_count > 0xFFFFFFFFULL) return 0ULL;
    return reserve_fake_pages(static_cast<uint32_t>(page_count));
}
/* The host stack test's fake allocator returns already-zeroed pages. These
   adapters model the managed lease API while address-space tests verify PTEs. */
uint64_t managed_alloc_phys_pages(uint64_t page_count) {
    return alloc_phys_pages(page_count);
}
bool managed_release_phys_pages(uint64_t physical_base, uint64_t page_count) {
    if (!page_aligned(physical_base) || physical_base == 0ULL || page_count == 0ULL)
        return false;
    if (managed_release_failures_remaining != 0u) {
        --managed_release_failures_remaining;
        return false;
    }
    return true;
}
} // namespace starlix::mem

namespace starlix::process {
bool process_is_registered(const StProcess* process) {
    for (uint32_t i = 0; i < registered_process_count; ++i)
        if (registered_processes[i] == process) return true;
    return false;
}
bool thread_is_registered(const StThread* thread) {
    for (uint32_t i = 0; i < registered_thread_count; ++i)
        if (registered_threads[i] == thread) return true;
    return false;
}
bool thread_prepare_user_interrupt_frame(
    StThread* thread, uint64_t base, uint64_t size
) {
    if (!thread_is_registered(thread) || base == 0ULL || size < 4096ULL ||
        thread->kernel_stack_top != base + size ||
        thread->saved_interrupt_rsp != 0ULL) return false;
    thread->kernel_stack_base = base;
    thread->kernel_stack_size = size;
    thread->saved_interrupt_rsp = thread->kernel_stack_top - 168ULL;
    return true;
}
} // namespace starlix::process

int main() {
    reset_fake();
    AddressSpacePlatform no_cpu_platform = valid_address_platform();
    no_cpu_platform.online_cpu_count = 0u;
    check(!address_space_manager_init(no_cpu_platform) &&
          std::strcmp(address_space_failure_reason(), "CPU_TOPOLOGY_UNAVAILABLE") == 0,
          "address-space manager rejects an unknown online CPU count");

    reset_fake();
    AddressSpacePlatform smp_platform = valid_address_platform();
    smp_platform.online_cpu_count = 2u;
    check(!address_space_manager_init(smp_platform) &&
          std::strcmp(address_space_failure_reason(),
                      "SMP_TLB_SHOOTDOWN_BACKEND_UNAVAILABLE") == 0,
          "address-space manager fails closed when multiple CPUs lack an SMP shootdown backend");

    reset_fake();
    fake_pages[0][KERNEL_MODULE_PML4_INDEX] = 0x6003ULL;
    const uint64_t occupied_module_slot = fake_pages[0][KERNEL_MODULE_PML4_INDEX];
    check(!address_space_manager_init(valid_address_platform()) &&
          fake_pages[0][KERNEL_MODULE_PML4_INDEX] == occupied_module_slot,
          "manager fails closed without overwriting firmware-owned PML4 slot 509");

    reset_fake();
    fake_pages[0][KERNEL_STACK_PML4_INDEX] = 0x7003ULL;
    const uint64_t occupied_stack_slot = fake_pages[0][KERNEL_STACK_PML4_INDEX];
    check(!address_space_manager_init(valid_address_platform()) &&
          fake_pages[0][KERNEL_STACK_PML4_INDEX] == occupied_stack_slot,
          "manager fails closed without overwriting a firmware-owned PML4 slot 510");

    reset_fake();
    check(address_space_manager_init(valid_address_platform()),
          "address-space manager reserves the dedicated high-half stack slot");
    check(address_space_online_cpu_count() == 1u,
          "address-space manager publishes the verified single-CPU topology");
    check(address_space_tlb_invalidation_generation() == 0ULL,
          "shared-kernel TLB invalidation generation starts at zero");
    check(kernel_stack_manager_init(KernelStackPlatform{
              fake_critical_enter, fake_critical_leave}),
          "kernel-stack manager initializes after physical and paging setup");
    check(kernel_stack_manager_ready(), "kernel-stack manager publishes ready state");

    AddressSpace first{};
    AddressSpace second{};
    check(address_space_create(first) && address_space_create(second),
          "process roots clone the kernel stack arena before stack allocation");
    volatile uint64_t* kernel_root = fake_map_table(address_space_kernel_root());
    volatile uint64_t* first_root = fake_map_table(first.cr3);
    volatile uint64_t* second_root = fake_map_table(second.cr3);
    check(kernel_root && first_root && second_root &&
          kernel_root[KERNEL_STACK_PML4_INDEX] == first_root[KERNEL_STACK_PML4_INDEX] &&
          kernel_root[KERNEL_STACK_PML4_INDEX] == second_root[KERNEL_STACK_PML4_INDEX],
          "all process roots share the supervisor kernel-stack page-table subtree");
    check(kernel_root &&
          (kernel_root[KERNEL_STACK_PML4_INDEX] & (PTE_PRESENT | PTE_WRITABLE | PTE_NX | PTE_USER)) ==
              (PTE_PRESENT | PTE_WRITABLE | PTE_NX),
          "reserved PML4 slot is present, writable, supervisor-only and NX");


    check(kernel_root && first_root && second_root &&
          kernel_root[KERNEL_MMIO_PML4_INDEX] != 0ULL &&
          kernel_root[KERNEL_MMIO_PML4_INDEX] ==
              first_root[KERNEL_MMIO_PML4_INDEX] &&
          kernel_root[KERNEL_MMIO_PML4_INDEX] ==
              second_root[KERNEL_MMIO_PML4_INDEX] &&
          (kernel_root[KERNEL_MMIO_PML4_INDEX] &
              (PTE_PRESENT | PTE_WRITABLE | PTE_NX | PTE_USER)) ==
              (PTE_PRESENT | PTE_WRITABLE | PTE_NX),
          "PML4 slot 508 is shared supervisor writable and NX");

    // Phase 9B: MMIO UC mappings keep the low identity alias on the
    // same cache type and restore the original identity attributes on unmap.
    const uint64_t mmio_test_physical = reserve_fake_pages(1u);
    fake_identity_map_page(mmio_test_physical, 2ULL | (1ULL << 63));

    constexpr uint64_t TEST_MMIO_UC_FLAGS =
        (1ULL << 3) | (1ULL << 4); // PAT index 3 in standard IA32_PAT.
    const uint64_t mmio_test_virtual =
        KERNEL_MMIO_REGION_BASE + 4ULL * PAGE_SIZE;

    check(!address_space_map_kernel_mmio_page(
              KERNEL_MMIO_REGION_BASE,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "MMIO lower guard page is rejected");

    check(!address_space_map_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical, 1ULL << 8),
          "unsupported MMIO cache PTE bits are rejected");

    check(address_space_map_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "UC MMIO page maps into the dedicated slot 508 arena");

    KernelMappingInfo mmio_info{};
    check(address_space_query_kernel_mapping(
              second.cr3, mmio_test_virtual, mmio_info) &&
          mmio_info.physical_address == mmio_test_physical &&
          mmio_info.page_size == PAGE_SIZE &&
          (mmio_info.permissions &
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_WRITE |
               KERNEL_MAPPING_EXECUTE | KERNEL_MAPPING_USER)) ==
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_WRITE),
          "MMIO alias is shared supervisor RW and NX");

    check(address_space_kernel_mmio_page_mapped(
              second.cr3, mmio_test_virtual,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "MMIO ownership and exact UC PTE are visible through cloned root");

    const uint32_t mmio_pt_slot =
        static_cast<uint32_t>((mmio_test_physical >> 21) & 0x1FFULL) - 128u;
    auto* mmio_identity_pt =
        mmio_pt_slot < 4u
            ? fake_map_table(fake_identity_pt_physical[mmio_pt_slot])
            : nullptr;

    const uint64_t mmio_identity_leaf =
        mmio_identity_pt
            ? mmio_identity_pt[(mmio_test_physical >> 12) & 0x1FFULL]
            : 0ULL;

    check(mmio_identity_pt &&
          (mmio_identity_leaf & ((1ULL << 3) | (1ULL << 4))) ==
              TEST_MMIO_UC_FLAGS,
          "identity alias receives the same UC PAT cache attributes");

    check(!address_space_map_kernel_mmio_page(
              mmio_test_virtual + PAGE_SIZE,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "same physical MMIO page cannot receive a second virtual alias");

    check(!address_space_map_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical + PAGE_SIZE, TEST_MMIO_UC_FLAGS),
          "occupied MMIO virtual address cannot be remapped");

    check(!address_space_unmap_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical, 0ULL),
          "MMIO unmap rejects a mismatched cache ownership token");

    check(!address_space_unmap_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical + PAGE_SIZE, TEST_MMIO_UC_FLAGS),
          "MMIO unmap rejects a mismatched physical page");

    check(address_space_kernel_mmio_page_mapped(
              address_space_kernel_root(), mmio_test_virtual,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "failed unmap attempts leave the valid MMIO mapping owned");

    check(address_space_unmap_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "exact MMIO unmap removes the virtual alias and restores ownership");

    check(!address_space_query_kernel_mapping(
              address_space_kernel_root(), mmio_test_virtual, mmio_info),
          "unmapped MMIO virtual page is no longer present");

    mmio_identity_pt =
        fake_map_table(fake_identity_pt_physical[mmio_pt_slot]);

    const uint64_t restored_identity_leaf =
        mmio_identity_pt
            ? mmio_identity_pt[(mmio_test_physical >> 12) & 0x1FFULL]
            : 0ULL;

    check(mmio_identity_pt &&
          (restored_identity_leaf &
              ((1ULL << 3) | (1ULL << 4) | (1ULL << 7))) == 0ULL,
          "identity alias restores its original WB cache attributes");

    check(fake_cache_flush_count >= 2u,
          "WBINVD backend flushes before UC transition and identity restore");

    check(!address_space_unmap_kernel_mmio_page(
              mmio_test_virtual,
              mmio_test_physical, TEST_MMIO_UC_FLAGS),
          "double MMIO unmap is rejected");

    // Phase 9B MMIO checks use the shared fake INVLPG counter.
    // Reset it so legacy stack tests retain their original isolated baseline.
    fake_invalidation_count = 0u;

    check(kernel_root && first_root && second_root &&
          kernel_root[KERNEL_MODULE_PML4_INDEX] == first_root[KERNEL_MODULE_PML4_INDEX] &&
          kernel_root[KERNEL_MODULE_PML4_INDEX] == second_root[KERNEL_MODULE_PML4_INDEX] &&
          (kernel_root[KERNEL_MODULE_PML4_INDEX] &
              (PTE_PRESENT | PTE_WRITABLE | PTE_NX | PTE_USER)) ==
              (PTE_PRESENT | PTE_WRITABLE),
          "module arena uses shared supervisor writable-capable slot 509 with leaf NX policy");

    KernelStack stack{};
    check(kernel_stack_allocate_default(stack),
          "default kernel stack receives four zeroed physical pages and a guarded VA slot");
    check(stack.magic == KERNEL_STACK_MAGIC && stack.page_count == KERNEL_STACK_DEFAULT_PAGES &&
          stack.size == 4ULL * PAGE_SIZE && stack.top == stack.virtual_base + stack.size,
          "stack descriptor contains exact virtual/physical allocation metadata");
    check((stack.virtual_base & (PAGE_SIZE - 1ULL)) == 0ULL &&
          stack.virtual_base == KERNEL_STACK_REGION_BASE + PAGE_SIZE,
          "first stack begins immediately after the initial unmapped guard page");
    check(address_space_kernel_stack_guard_unmapped(first.cr3,
              stack.virtual_base - PAGE_SIZE) &&
          address_space_kernel_stack_guard_unmapped(first.cr3, stack.top),
          "lower and upper guard pages remain unmapped");
    check(address_space_kernel_stack_range_mapped(first.cr3, stack.virtual_base,
              stack.physical_base, stack.page_count),
          "all stack pages map to their exact contiguous physical frames");
    kernel_root[KERNEL_STACK_PML4_INDEX] |= (1ULL << 5); // Hardware Accessed bit.
    check(address_space_kernel_stack_range_mapped(first.cr3, stack.virtual_base,
              stack.physical_base, stack.page_count),
          "shared stack validator tolerates hardware-set PML4 Accessed bit");
    volatile uint64_t* first_stack_leaf = fake_stack_leaf(first.cr3, stack.virtual_base);
    check(first_stack_leaf &&
          ((*first_stack_leaf & (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) ==
              (PTE_PRESENT | PTE_WRITABLE | PTE_NX)) &&
          ((*first_stack_leaf & PTE_ADDRESS_MASK) == stack.physical_base),
          "stack leaf PTE is supervisor writable NX and references its physical frame");
    check(fake_invalidation_count == stack.page_count,
          "mapping commit invalidates every newly installed stack virtual page");
    check(address_space_kernel_stack_range_mapped(second.cr3, stack.virtual_base,
              stack.physical_base, stack.page_count),
          "stack mapping is visible through another process root");
    check(!address_space_kernel_stack_range_mapped(first.cr3, stack.virtual_base,
              stack.physical_base + PAGE_SIZE, stack.page_count),
          "stack mapping validator rejects the wrong physical backing");
    check(!address_space_map_kernel_stack_range(stack.virtual_base,
              stack.physical_base + 0x100000ULL, stack.page_count),
          "duplicate virtual stack mapping is rejected");
    check(!address_space_map_kernel_stack_range(KERNEL_STACK_REGION_END,
              stack.physical_base, stack.page_count),
          "virtual range outside the reserved PML4 slot is rejected");
    check(!address_space_map_kernel_stack_range(stack.virtual_base + 1ULL,
              stack.physical_base, stack.page_count),
          "unaligned virtual stack mapping is rejected");
    check(!address_space_map_kernel_stack_range(stack.virtual_base + PAGE_SIZE,
              stack.physical_base, stack.page_count),
          "mapping across an already occupied stack page is rejected transactionally");

    StProcess process1{};
    process1.pid = 10;
    process1.page_directory_cr3 = first.cr3;
    process1.state = PROCESS_STATE_READY;
    registered_processes[registered_process_count++] = &process1;
    StThread thread1{};
    thread1.tid = 100;
    thread1.priority = 8;
    thread1.state = THREAD_STATE_READY;
    thread1.kernel_stack_top = stack.top;
    thread1.parent_process = &process1;
    registered_threads[registered_thread_count++] = &thread1;
    check(kernel_stack_bind_thread(stack, &thread1),
          "allocated stack binds to a registered READY user thread");
    check(thread1.kernel_stack_base == stack.virtual_base &&
          thread1.kernel_stack_size == stack.size &&
          thread1.kernel_stack_physical_base == stack.physical_base &&
          thread1.kernel_stack_allocation_id == stack.allocation_id &&
          thread1.saved_interrupt_rsp != 0ULL,
          "thread stores virtual stack, physical backing, allocation ID and initial frame");
    check(kernel_stack_thread_valid(&thread1, process1.page_directory_cr3),
          "bound thread passes per-process stack ownership and mapping validation");
    check(!kernel_stack_bind_thread(stack, &thread1),
          "a stack cannot be bound to the same thread twice");

    KernelStack forged = stack;
    forged.physical_base += PAGE_SIZE;
    check(!kernel_stack_bind_thread(forged, &thread1),
          "forged stack descriptor is rejected by registry identity");
    StThread foreign_thread{};
    foreign_thread.tid = 101;
    foreign_thread.kernel_stack_top = stack.top;
    foreign_thread.parent_process = &process1;
    check(!kernel_stack_bind_thread(stack, &foreign_thread),
          "foreign thread pointer is rejected before stack binding");
    check(!kernel_stack_thread_valid(&foreign_thread, process1.page_directory_cr3),
          "unregistered thread cannot claim a managed kernel stack");

    KernelStack larger{};
    check(kernel_stack_allocate(8u, larger),
          "allocator supports a larger eight-page stack");
    check(larger.virtual_base >= stack.top + 2ULL * PAGE_SIZE,
          "separate stacks retain both guard pages and a gap");
    check(fake_invalidation_count == stack.page_count + larger.page_count,
          "larger stack mapping also invalidates every newly mapped page");
    check(address_space_kernel_stack_guard_unmapped(first.cr3,
              larger.virtual_base - PAGE_SIZE) &&
          address_space_kernel_stack_guard_unmapped(first.cr3, larger.top),
          "larger stack also has two absent guard pages");

    StProcess wrong_process{};
    wrong_process.pid = 11;
    wrong_process.page_directory_cr3 = second.cr3;
    wrong_process.state = PROCESS_STATE_READY;
    registered_processes[registered_process_count++] = &wrong_process;
    StThread wrong_root_thread{};
    wrong_root_thread.tid = 102;
    wrong_root_thread.state = THREAD_STATE_READY;
    wrong_root_thread.kernel_stack_top = stack.top;
    wrong_root_thread.parent_process = &wrong_process;
    registered_threads[registered_thread_count++] = &wrong_root_thread;
    check(!kernel_stack_thread_valid(&thread1, second.cr3),
          "thread stack cannot be validated against another process root");
    check(kernel_stack_count() == 2u,
          "stack registry reports the two live stack allocations");

    const uint32_t before_release_invalidations = fake_invalidation_count;
    const uint32_t expected_release_pages = stack.page_count + larger.page_count;
    managed_release_failures_remaining = 1u;
    check(!kernel_stack_release(larger),
          "managed-page release failure preserves a retryable stack record");
    check(!address_space_kernel_stack_range_mapped(
              first.cr3, larger.virtual_base, larger.physical_base,
              larger.page_count) && kernel_stack_count() == 2u,
          "failed release keeps ownership but leaves the stack mapping removed");
    check(kernel_stack_release(larger),
          "retry completes release without unmapping the stack twice");
    check(!address_space_kernel_stack_range_mapped(
              first.cr3, larger.virtual_base, larger.physical_base,
              larger.page_count),
          "released unbound stack no longer has present PTEs");
    check(kernel_stack_release(stack, &thread1),
          "bound but unpublished READY thread stack can be detached and released");
    check(thread1.kernel_stack_base == 0ULL && thread1.kernel_stack_size == 0ULL &&
          thread1.kernel_stack_top == 0ULL && thread1.saved_interrupt_rsp == 0ULL &&
          thread1.kernel_stack_allocation_id == 0u &&
          thread1.kernel_stack_physical_base == 0ULL,
          "release clears every kernel-stack field from the READY thread");
    check(!address_space_kernel_stack_range_mapped(
              second.cr3, stack.virtual_base, stack.physical_base,
              stack.page_count),
          "released bound stack is absent through every shared process root");
    check(fake_invalidation_count == before_release_invalidations +
              expected_release_pages && kernel_stack_count() == 0u,
          "release invalidates all pages once and retires both stack records");

    KernelStack invalid{};
    check(!kernel_stack_allocate(0u, invalid), "zero-page stack allocation is rejected");
    check(!kernel_stack_allocate(3u, invalid), "undersized stack allocation is rejected");
    check(!kernel_stack_allocate(17u, invalid), "oversized stack allocation is rejected");

    // Phase 4: create a fake, read-only/NX identity alias for an executable
    // module page, then exercise the shared supervisor module arena.
    constexpr uint64_t TEST_NX = 1ULL << 63;
    const uint64_t executable_physical = reserve_fake_pages(1u);
    fake_identity_map_page(executable_physical, TEST_NX); // RO + NX identity alias.
    const uint64_t executable_virtual = KERNEL_MODULE_REGION_BASE + PAGE_SIZE;
    check(address_space_map_kernel_module_page(executable_virtual,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE),
          "RO+NX identity page can receive a separate supervisor RO+X module mapping");
    KernelMappingInfo executable_info{};
    check(address_space_query_kernel_mapping(first.cr3, executable_virtual,
              executable_info) &&
          executable_info.physical_address == executable_physical &&
          executable_info.page_size == PAGE_SIZE &&
          (executable_info.permissions &
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE |
               KERNEL_MAPPING_USER)) ==
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_EXECUTE),
          "module code mapping is readable, executable, supervisor-only and not writable");
    check(address_space_kernel_module_page_mapped(second.cr3, executable_virtual,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE),
          "module mapping is visible through every cloned process root");
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              executable_physical, executable_info) &&
          executable_info.physical_address == executable_physical &&
          (executable_info.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) == 0u,
          "identity alias for executable module page remains read-only and NX");
    check(!address_space_map_kernel_module_page(executable_virtual + PAGE_SIZE,
              executable_physical, KERNEL_MODULE_MAP_WRITE),
          "same physical page cannot be remapped with conflicting module permissions");
    check(!address_space_map_kernel_module_page(executable_virtual + PAGE_SIZE,
              reserve_fake_pages(1u), KERNEL_MODULE_MAP_FLAGS_ALL),
          "module mapping API rejects a writable-and-executable request");
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_BASE,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE) &&
          address_space_kernel_module_guard_unmapped(first.cr3,
              KERNEL_MODULE_REGION_BASE),
          "lower module guard page is never mapped");
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_END - PAGE_SIZE,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE) &&
          address_space_kernel_module_guard_unmapped(second.cr3,
              KERNEL_MODULE_REGION_END - PAGE_SIZE),
          "upper module guard page is never mapped");
    check(!address_space_map_kernel_module_page(executable_virtual + 1ULL,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE),
          "unaligned module virtual pages are rejected");
    check(address_space_unmap_kernel_module_page(executable_virtual,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE) &&
          !address_space_kernel_module_page_mapped(first.cr3, executable_virtual,
              executable_physical, KERNEL_MODULE_MAP_EXECUTE),
          "exact module mapping can be unmapped and is absent from cloned roots");

    const uint64_t writable_identity_physical = reserve_fake_pages(1u);
    fake_identity_map_page(writable_identity_physical, 2ULL | TEST_NX);
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_BASE + 2ULL * PAGE_SIZE,
              writable_identity_physical, KERNEL_MODULE_MAP_EXECUTE),
          "executable mapping is rejected while the identity alias remains writable");

    const uint64_t executable_identity_physical = reserve_fake_pages(1u);
    fake_identity_map_page(executable_identity_physical, 0ULL); // RO + executable identity alias.
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_BASE + 3ULL * PAGE_SIZE,
              executable_identity_physical, KERNEL_MODULE_MAP_WRITE),
          "writable mapping is rejected while the identity alias remains executable");

    const uint64_t wx_identity_physical = reserve_fake_pages(1u);
    fake_identity_map_page(wx_identity_physical, 2ULL); // RW + executable identity alias.
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_BASE + 4ULL * PAGE_SIZE,
              wx_identity_physical, 0u),
          "pre-existing writable-executable identity aliases are rejected");

    const uint64_t writable_data_physical = reserve_fake_pages(1u);
    fake_identity_map_page(writable_data_physical, 2ULL | TEST_NX);
    const uint64_t writable_data_virtual = KERNEL_MODULE_REGION_BASE + 5ULL * PAGE_SIZE;
    check(address_space_map_kernel_module_page(writable_data_virtual,
              writable_data_physical, KERNEL_MODULE_MAP_WRITE),
          "RW+NX data mapping is accepted when the identity alias is also RW+NX");
    KernelMappingInfo data_info{};
    check(address_space_query_kernel_mapping(second.cr3, writable_data_virtual,
              data_info) &&
          data_info.physical_address == writable_data_physical &&
          (data_info.permissions &
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE |
               KERNEL_MAPPING_USER)) ==
              (KERNEL_MAPPING_READ | KERNEL_MAPPING_WRITE),
          "module data mapping is readable, writable, supervisor-only and NX");
    check(address_space_unmap_kernel_module_page(writable_data_virtual,
              writable_data_physical, KERNEL_MODULE_MAP_WRITE),
          "exact RW+NX module data mapping can be removed");
    check(fake_invalidation_count >= 2u,
          "module mapping and unmapping invalidate their virtual pages");

    // Identity aliases may be inherited as UEFI 1 GiB / 2 MiB huge pages.
    // The query path must report their effective permissions without assuming
    // a non-zero huge-page base, and reject reserved low address bits.
    auto* identity_pdpt = fake_map_table(fake_identity_pdpt_physical);
    const uint64_t huge_1g_physical = reserve_fake_pages(1u);
    identity_pdpt[0] = (1ULL | 2ULL | (1ULL << 7) | TEST_NX);
    KernelMappingInfo huge_info{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              huge_1g_physical, huge_info) &&
          huge_info.physical_address == huge_1g_physical &&
          huge_info.page_size == (1u << 30) &&
          (huge_info.permissions & (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "identity alias query correctly resolves a 1 GiB RW+NX huge page at physical base zero");
    check(!address_space_map_kernel_module_page(KERNEL_MODULE_REGION_BASE +
              6ULL * PAGE_SIZE, huge_1g_physical, KERNEL_MODULE_MAP_EXECUTE),
          "executable module page is rejected when a 1 GiB writable identity alias exists");

    identity_pdpt[0] = fake_identity_pd_physical | 0x3ULL;
    auto* identity_pd = fake_map_table(fake_identity_pd_physical);
    const uint64_t huge_2m_physical = reserve_fake_pages(1u);
    identity_pd[128] = 0x10000000ULL | 1ULL | 2ULL | (1ULL << 7) | TEST_NX;
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              huge_2m_physical, huge_info) &&
          huge_info.physical_address == huge_2m_physical &&
          huge_info.page_size == (1u << 21) &&
          (huge_info.permissions & (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "identity alias query correctly resolves a 2 MiB RW+NX huge page");
    identity_pd[128] |= (1ULL << 13); // Reserved in a 2 MiB page entry.
    check(!address_space_query_kernel_mapping(address_space_kernel_root(),
              huge_2m_physical, huge_info),
          "malformed huge-page reserved address bits fail closed");

    uint32_t remaining = KERNEL_STACK_CAPACITY - kernel_stack_count();
    bool capacity_filled = true;
    for (uint32_t i = 0; i < remaining; ++i) {
        KernelStack extra{};
        if (!kernel_stack_allocate_default(extra)) {
            capacity_filled = false;
            break;
        }
    }
    check(capacity_filled && kernel_stack_count() == KERNEL_STACK_CAPACITY,
          "boot-lifetime stack registry reaches its configured capacity");
    KernelStack overflow{};
    check(!kernel_stack_allocate_default(overflow),
          "stack registry rejects overflow without allocating more pages");

    // Phase 5: make the low identity alias of a driver code page RO+NX before
    // mapping it RO+X in the dedicated external-module arena. Restore the
    // identity alias only after the module alias has been removed.
    reset_fake();
    check(address_space_manager_init(valid_address_platform()),
          "identity permission manager initializes on a clean root");
    const uint64_t identity_code_page = reserve_fake_pages(1u);
    fake_identity_map_page(identity_code_page, 2ULL | (1ULL << 63));
    AddressSpace identity_clone{};
    check(address_space_create(identity_clone),
          "identity permission test creates a clone sharing kernel slot zero");
    KernelMappingInfo identity_before{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              identity_code_page, identity_before) &&
          identity_before.physical_address == identity_code_page &&
          identity_before.page_size == PAGE_SIZE &&
          (identity_before.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "managed code page begins as an RW+NX identity alias");
    const uint64_t identity_generation_before =
        address_space_tlb_invalidation_generation();
    const uint32_t identity_invalidations_before = fake_invalidation_count;
    check(address_space_set_kernel_identity_page_permissions(
              identity_code_page, KERNEL_MAPPING_WRITE, 0u),
          "RW+NX identity alias is transactionally reduced to RO+NX");
    check(address_space_tlb_invalidation_generation() ==
              identity_generation_before +
                  static_cast<uint64_t>(fake_invalidation_count - identity_invalidations_before),
          "shared identity permission changes advance the local invalidation generation");
    KernelMappingInfo identity_protected{};
    check(address_space_query_kernel_mapping(identity_clone.cr3,
              identity_code_page, identity_protected) &&
          identity_protected.physical_address == identity_code_page &&
          identity_protected.page_size == PAGE_SIZE &&
          (identity_protected.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) == 0u,
          "RO+NX identity protection is visible through cloned process roots");
    check(!address_space_set_kernel_identity_page_permissions(
              identity_code_page, KERNEL_MAPPING_WRITE, 0u),
          "identity permission update rejects a stale expected-permission token");
    const uint64_t identity_code_va = KERNEL_MODULE_REGION_BASE + 32ULL * PAGE_SIZE;
    check(address_space_map_kernel_module_page(identity_code_va,
              identity_code_page, KERNEL_MODULE_MAP_EXECUTE),
          "protected identity page can receive a separate RO+X module alias");
    check(!address_space_set_kernel_identity_page_permissions(
              identity_code_page, 0u, KERNEL_MAPPING_WRITE),
          "identity alias cannot become writable while an executable module alias exists");
    check(address_space_unmap_kernel_module_page(identity_code_va,
              identity_code_page, KERNEL_MODULE_MAP_EXECUTE),
          "separate executable module alias can be removed before identity restoration");
    check(address_space_set_kernel_identity_page_permissions(
              identity_code_page, 0u, KERNEL_MAPPING_WRITE),
          "RO+NX identity alias can be restored to RW+NX after module unmap");
    KernelMappingInfo identity_restored{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              identity_code_page, identity_restored) &&
          (identity_restored.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "restored identity page is RW+NX and never writable-executable");

    // Demote a 2 MiB huge identity page to 4 KiB leaves, preserving the
    // large-page PAT bit as the 4 KiB PTE PAT bit and retaining sibling access.
    reset_fake();
    check(address_space_manager_init(valid_address_platform()),
          "2 MiB identity-split manager initializes");
    const uint64_t split2_pdpt_phys = reserve_fake_pages(1u);
    const uint64_t split2_pd_phys = reserve_fake_pages(1u);
    fake_pages[0][0] = split2_pdpt_phys | 0x3ULL;
    auto* split2_pdpt = fake_map_table(split2_pdpt_phys);
    auto* split2_pd = fake_map_table(split2_pd_phys);
    split2_pdpt[0] = split2_pd_phys | 0x3ULL;
    constexpr uint64_t SPLIT_2M_BASE = 0x10000000ULL;
    constexpr uint64_t SPLIT_2M_FLAGS = 1ULL | 2ULL | (1ULL << 3) |
        (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) |
        (1ULL << 8) | (1ULL << 12) | (1ULL << 63);
    split2_pd[128] = SPLIT_2M_BASE | SPLIT_2M_FLAGS;
    const uint64_t split2_target = SPLIT_2M_BASE + 0x123000ULL;
    KernelMappingInfo split2_before{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split2_target, split2_before) &&
          split2_before.page_size == (1u << 21) &&
          split2_before.physical_address == split2_target,
          "2 MiB huge identity mapping is discovered before demotion");
    const bool split2_protected = address_space_set_kernel_identity_page_permissions(
              split2_target, KERNEL_MAPPING_WRITE, 0u);
    check(split2_protected,
          "2 MiB huge identity mapping is demoted and target page made RO+NX");
    KernelMappingInfo split2_after{};
    KernelMappingInfo split2_sibling{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split2_target, split2_after) &&
          split2_after.page_size == PAGE_SIZE &&
          split2_after.physical_address == split2_target &&
          (split2_after.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) == 0u,
          "demoted 2 MiB target resolves to a protected 4 KiB leaf");
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split2_target + PAGE_SIZE, split2_sibling) &&
          split2_sibling.page_size == PAGE_SIZE &&
          split2_sibling.physical_address == split2_target + PAGE_SIZE &&
          (split2_sibling.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "neighboring 2 MiB split leaf retains its original RW+NX permissions");
    auto* split2_pt = fake_map_table(split2_pd[128] & PTE_ADDRESS_MASK);
    check(split2_pt && (split2_pt[(split2_target >> 12) & 0x1FFULL] & (1ULL << 7)) != 0ULL,
          "2 MiB large-page PAT attribute is converted to the 4 KiB PAT bit");

    // Demote a 1 GiB page through a 2 MiB page table and then a 4 KiB PT.
    reset_fake();
    check(address_space_manager_init(valid_address_platform()),
          "1 GiB identity-split manager initializes");
    const uint64_t split1_pdpt_phys = reserve_fake_pages(1u);
    fake_pages[0][0] = split1_pdpt_phys | 0x3ULL;
    auto* split1_pdpt = fake_map_table(split1_pdpt_phys);
    split1_pdpt[0] = 1ULL | 2ULL | (1ULL << 3) | (1ULL << 4) |
        (1ULL << 5) | (1ULL << 6) | (1ULL << 7) | (1ULL << 8) |
        (1ULL << 12) | (1ULL << 63);
    const uint64_t split1_target = 0x10005000ULL;
    KernelMappingInfo split1_before{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split1_target, split1_before) &&
          split1_before.page_size == (1u << 30) &&
          split1_before.physical_address == split1_target,
          "1 GiB huge identity mapping is discovered before demotion");
    const bool split1_protected = address_space_set_kernel_identity_page_permissions(
              split1_target, KERNEL_MAPPING_WRITE, 0u);
    check(split1_protected,
          "1 GiB huge identity mapping is demoted through 2 MiB to 4 KiB");
    KernelMappingInfo split1_after{};
    KernelMappingInfo split1_sibling{};
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split1_target, split1_after) &&
          split1_after.page_size == PAGE_SIZE &&
          split1_after.physical_address == split1_target &&
          (split1_after.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) == 0u,
          "demoted 1 GiB target resolves to a protected 4 KiB leaf");
    check(address_space_query_kernel_mapping(address_space_kernel_root(),
              split1_target + (2ULL << 20), split1_sibling) &&
          split1_sibling.page_size == (1u << 21) &&
          (split1_sibling.permissions &
              (KERNEL_MAPPING_WRITE | KERNEL_MAPPING_EXECUTE)) ==
              KERNEL_MAPPING_WRITE,
          "other 1 GiB subranges retain 2 MiB RW+NX mappings");
    auto* split1_pd = fake_map_table(split1_pdpt[0] & PTE_ADDRESS_MASK);
    auto* split1_pt = split1_pd
        ? fake_map_table(split1_pd[128] & PTE_ADDRESS_MASK) : nullptr;
    check(split1_pt && (split1_pt[(split1_target >> 12) & 0x1FFULL] & (1ULL << 7)) != 0ULL,
          "1 GiB to 2 MiB to 4 KiB demotion preserves PAT semantics");
    check(fake_invalidation_count >= 3u,
          "huge-page demotions and permission commit invalidate the target TLB entry");

    std::printf("KERNEL_STACK_AND_MODULE_MAPPING_SELFTEST: %s (%u checks, %u failures)\n",
                failures == 0u ? "PASS" : "FAIL", checks, failures);
    return failures == 0u ? 0 : 1;
}
