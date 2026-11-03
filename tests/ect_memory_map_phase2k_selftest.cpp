#include "../kernel/src/loader/ect_memory_map.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace starlix::loader::ect;
constexpr uint64_t PAGE = 4096ULL;
constexpr uint64_t PHYS_BASE = 0x30000000ULL;
constexpr size_t PAGE_CAPACITY = 128u;
constexpr size_t MAP_CAPACITY = 256u;

struct FakeMap {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint32_t permissions;
    bool active;
};

struct FakeBackend {
    std::array<uint8_t, PAGE_CAPACITY * PAGE> memory{};
    std::array<bool, PAGE_CAPACITY> allocated{};
    std::array<FakeMap, MAP_CAPACITY> mappings{};
    uint32_t allocation_calls = 0;
    uint32_t release_calls = 0;
    uint32_t map_calls = 0;
    uint32_t unmap_calls = 0;
    uint32_t fail_allocation_at = 0;
    uint32_t fail_map_at = 0;
    uint32_t fail_unmap_at = 0;
    bool fail_physical_view = false;
};

unsigned passed = 0;
unsigned failed = 0;

void check(bool condition, const char* name) {
    if (condition) ++passed;
    else {
        ++failed;
        std::cerr << "FAIL: " << name << "\n";
    }
}

u64 allocate_pages(void* raw, u64 count) {
    auto& state = *static_cast<FakeBackend*>(raw);
    ++state.allocation_calls;
    if (state.fail_allocation_at == state.allocation_calls || count == 0u ||
        count > PAGE_CAPACITY) return 0ULL;
    for (size_t start = 0; start + count <= PAGE_CAPACITY; ++start) {
        bool available = true;
        for (u64 i = 0; i < count; ++i)
            if (state.allocated[start + i]) available = false;
        if (!available) continue;
        for (u64 i = 0; i < count; ++i) state.allocated[start + i] = true;
        return PHYS_BASE + static_cast<u64>(start) * PAGE;
    }
    return 0ULL;
}

bool release_pages(void* raw, u64 base, u64 count) {
    auto& state = *static_cast<FakeBackend*>(raw);
    ++state.release_calls;
    if (base < PHYS_BASE || ((base - PHYS_BASE) & (PAGE - 1ULL)) != 0u ||
        count == 0u) return false;
    const u64 first = (base - PHYS_BASE) / PAGE;
    if (first + count > PAGE_CAPACITY) return false;
    for (u64 i = 0; i < count; ++i)
        if (!state.allocated[first + i]) return false;
    for (u64 i = 0; i < count; ++i) state.allocated[first + i] = false;
    return true;
}

void* physical_to_virtual(void* raw, u64 base, u64 bytes) {
    auto& state = *static_cast<FakeBackend*>(raw);
    if (state.fail_physical_view || base < PHYS_BASE || bytes == 0u ||
        ((base - PHYS_BASE) & (PAGE - 1ULL)) != 0u) return nullptr;
    const u64 offset = base - PHYS_BASE;
    if (offset > state.memory.size() || bytes > state.memory.size() - offset)
        return nullptr;
    return state.memory.data() + offset;
}

bool map_page(void* raw, void*, u64 va, u64 pa, u32 permissions) {
    auto& state = *static_cast<FakeBackend*>(raw);
    ++state.map_calls;
    if (state.fail_map_at == state.map_calls) return false;
    for (const auto& entry : state.mappings)
        if (entry.active && entry.virtual_address == va) return false;
    for (auto& entry : state.mappings) {
        if (!entry.active) {
            entry = FakeMap{va, pa, permissions, true};
            return true;
        }
    }
    return false;
}

bool unmap_page(void* raw, void*, u64 va) {
    auto& state = *static_cast<FakeBackend*>(raw);
    ++state.unmap_calls;
    if (state.fail_unmap_at == state.unmap_calls) return false;
    for (auto& entry : state.mappings) {
        if (entry.active && entry.virtual_address == va) {
            entry.active = false;
            return true;
        }
    }
    return false;
}

MappingBackend backend_for(FakeBackend& state) {
    MappingBackend backend{};
    backend.context = &state;
    backend.allocate_pages = allocate_pages;
    backend.release_pages = release_pages;
    backend.physical_to_virtual = physical_to_virtual;
    backend.map_user_page = map_page;
    backend.unmap_user_page = unmap_page;
    return backend;
}

uint32_t active_mappings(const FakeBackend& state) {
    uint32_t count = 0u;
    for (const auto& mapping : state.mappings)
        if (mapping.active) ++count;
    return count;
}
uint32_t allocated_pages(const FakeBackend& state) {
    uint32_t count = 0u;
    for (bool value : state.allocated) if (value) ++count;
    return count;
}

const FakeMap* find_mapping(const FakeBackend& state, uint64_t va) {
    for (const auto& mapping : state.mappings)
        if (mapping.active && mapping.virtual_address == va) return &mapping;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: ect_memory_map_phase2k_selftest <stlcs.ect>\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "cannot open stlcs.ect\n";
        return 2;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    LoadPlan plan{};
    check(validate_image(image.data(), image.size(), plan), "input ECT validates");

    // Success path: section bytes are copied, tails stay zero, and W^X flags survive.
    {
        FakeBackend fake{};
        const MappingBackend backend = backend_for(fake);
        MappedImage mapped{};
        const bool ok = map_image_transactional(image.data(), image.size(),
                                                backend, nullptr, 0x12345000ULL, mapped);
        check(ok, "valid image maps");
        check(mapped.magic == MAPPED_IMAGE_MAGIC && mapped.section_count == 2u &&
              mapped.total_pages == 2u && mapped.entry_point == plan.entry_point,
              "mapping handle reports validated image metadata");
        check(active_mappings(fake) == 2u && allocated_pages(fake) == 2u,
              "success path owns exactly two mapped pages");
        const FakeMap* text = find_mapping(fake, plan.sections[0].virtual_address);
        const FakeMap* rodata = find_mapping(fake, plan.sections[1].virtual_address);
        check(text && text->permissions == (SECTION_READ | SECTION_EXEC),
              ".text mapped read+execute");
        check(rodata && rodata->permissions == SECTION_READ,
              ".rodata mapped read-only");
        if (text) {
            const uint64_t offset = text->physical_address - PHYS_BASE;
            check(fake.memory[offset] == image[plan.sections[0].file_offset],
                  ".text first byte copied");
            check(fake.memory[offset + plan.sections[0].byte_size - 1u] ==
                  image[plan.sections[0].file_offset + plan.sections[0].byte_size - 1u],
                  ".text final byte copied");
            check(fake.memory[offset + PAGE - 1u] == 0u,
                  ".text page tail zero-filled");
        } else {
            check(false, "text mapping exists for byte copy checks");
            check(false, "text mapping exists for tail check");
        }
        check(!unmap_image_transactional(backend, nullptr, 0x22345000ULL, mapped),
              "mapping handle cannot be released through a different address space");
        check(active_mappings(fake) == 2u && allocated_pages(fake) == 2u,
              "wrong-owner unmap has no side effects");
        check(unmap_image_transactional(backend, nullptr, 0x12345000ULL, mapped),
              "explicit unmap succeeds");
        check(mapped.magic == 0u && allocated_pages(fake) == 0u &&
              active_mappings(fake) == 0u,
              "explicit unmap releases mappings and all page leases");
    }

    // Mapping failure after one successful section must unmap and release both runs.
    {
        FakeBackend fake{};
        fake.fail_map_at = 2u;
        const MappingBackend backend = backend_for(fake);
        MappedImage mapped{};
        const bool ok = map_image_transactional(image.data(), image.size(),
                                                backend, nullptr, 0x12345000ULL, mapped);
        check(!ok, "injected second map failure is rejected");
        check(mapped.magic == 0u && active_mappings(fake) == 0u &&
              allocated_pages(fake) == 0u,
              "second map failure rolls back all pages");
    }

    // Allocation failure during the second section rolls back the first section.
    {
        FakeBackend fake{};
        fake.fail_allocation_at = 2u;
        const MappingBackend backend = backend_for(fake);
        MappedImage mapped{};
        const bool ok = map_image_transactional(image.data(), image.size(),
                                                backend, nullptr, 0x12345000ULL, mapped);
        check(!ok, "injected second allocation failure is rejected");
        check(mapped.magic == 0u && active_mappings(fake) == 0u &&
              allocated_pages(fake) == 0u,
              "allocation failure rolls back earlier section");
    }

    // No mapped frame may be released if an unmap fails; returned handle enables retry.
    {
        FakeBackend fake{};
        fake.fail_unmap_at = 1u;
        const MappingBackend backend = backend_for(fake);
        MappedImage mapped{};
        check(map_image_transactional(image.data(), image.size(), backend,
                                      nullptr, 0x12345000ULL, mapped), "prepare unmap retry case");
        check(!unmap_image_transactional(backend, nullptr, 0x12345000ULL, mapped),
              "injected unmap failure is reported");
        check(mapped.magic == MAPPED_IMAGE_MAGIC && allocated_pages(fake) == 1u,
              "failed unmap retains physical ownership");
        fake.fail_unmap_at = 0u;
        check(unmap_image_transactional(backend, nullptr, 0x12345000ULL, mapped),
              "unmap retry succeeds");
        check(mapped.magic == 0u && allocated_pages(fake) == 0u &&
              active_mappings(fake) == 0u,
              "unmap retry releases all resources");
    }

    // A malformed image is rejected before it can allocate or map memory.
    {
        FakeBackend fake{};
        const MappingBackend backend = backend_for(fake);
        std::vector<uint8_t> invalid = image;
        invalid[0] = 'Q';
        MappedImage mapped{};
        check(!map_image_transactional(invalid.data(), invalid.size(),
                                       backend, nullptr, 0x12345000ULL, mapped),
              "invalid image rejected before mapping");
        check(fake.allocation_calls == 0u && fake.map_calls == 0u &&
              allocated_pages(fake) == 0u,
              "invalid image has no allocator side effects");
    }

    // Physical-view failure returns the section allocation without leaving mappings.
    {
        FakeBackend fake{};
        fake.fail_physical_view = true;
        const MappingBackend backend = backend_for(fake);
        MappedImage mapped{};
        check(!map_image_transactional(image.data(), image.size(),
                                       backend, nullptr, 0x12345000ULL, mapped),
              "physical mapping view failure is rejected");
        check(mapped.magic == 0u && allocated_pages(fake) == 0u &&
              active_mappings(fake) == 0u,
              "physical view failure releases the acquired run");
    }

    std::cout << "ECT Phase 2K memory-map self-test: " << passed
              << " passed, " << failed << " failed\n";
    return failed == 0u ? 0 : 1;
}
