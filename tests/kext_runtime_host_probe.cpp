// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STARLIX_PRIMITIVE_TYPES_DEFINED 1
#include "loader/kext_runtime.hpp"
#include "driver/gpu_kext_abi.hpp"

namespace {
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kPages = starlix::loader::KEXT_RUNTIME_PAGE_COUNT;
constexpr uint64_t kCapacity = starlix::loader::KEXT_RUNTIME_IMAGE_CAPACITY;

struct HostMemory {
    int fd{-1};
    uint8_t* identity{};
    uint8_t* module{};
    uint32_t identity_writable[kPages]{};
    uint32_t module_flags[kPages]{};
    uint32_t identity_protect_calls{};
    uint32_t module_map_calls{};
    uint32_t module_unmap_calls{};
    uint32_t release_calls{};
    uint32_t allocation_attempts{};
    bool released{};
    bool leased{};
};

bool check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS:" : "FAIL:", message);
    return ok;
}

uint64_t allocate_pages(void* opaque, uint64_t pages) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory) return 0;
    ++memory->allocation_attempts;
    if (pages != kPages || !memory->identity || memory->leased)
        return 0;
    memory->leased = true;
    return reinterpret_cast<uintptr_t>(memory->identity);
}

bool release_pages(void* opaque, uint64_t physical, uint64_t pages) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory || !memory->identity || pages != kPages ||
        physical != reinterpret_cast<uintptr_t>(memory->identity)) return false;
    for (uint64_t i = 0; i < pages; ++i) {
        if (!memory->identity_writable[i] || memory->module_flags[i] !=
                starlix::loader::KEXT_RUNTIME_PAGE_UNMAPPED) return false;
    }
    for (uint64_t i = 0; i < kCapacity; ++i)
        if (memory->identity[i] != 0u) return false;
    ++memory->release_calls;
    memory->released = true;
    return true;
}

bool set_identity_permissions(void* opaque, uint64_t physical,
                              uint32_t expected, uint32_t target) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory || !memory->identity || physical <
            reinterpret_cast<uintptr_t>(memory->identity)) return false;
    const uint64_t offset = physical - reinterpret_cast<uintptr_t>(memory->identity);
    if ((offset & (kPageSize - 1u)) != 0u || offset >= kCapacity) return false;
    const uint64_t page = offset / kPageSize;
    const uint32_t current = memory->identity_writable[page]
        ? starlix::mem::KERNEL_MAPPING_WRITE : 0u;
    if (current != expected ||
        (target != 0u && target != starlix::mem::KERNEL_MAPPING_WRITE)) return false;
    const int prot = PROT_READ |
        (target == starlix::mem::KERNEL_MAPPING_WRITE ? PROT_WRITE : 0);
    if (mprotect(memory->identity + offset, kPageSize, prot) != 0) return false;
    memory->identity_writable[page] =
        target == starlix::mem::KERNEL_MAPPING_WRITE ? 1u : 0u;
    ++memory->identity_protect_calls;
    return true;
}

bool guard_unmapped(void*, uint64_t) { return true; }

bool map_module_page(void* opaque, uint64_t virtual_address,
                     uint64_t physical, uint32_t flags) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory || !memory->module || !memory->identity ||
        physical < reinterpret_cast<uintptr_t>(memory->identity) ||
        virtual_address < reinterpret_cast<uintptr_t>(memory->module) ||
        flags > 2u) return false;
    const uint64_t physical_offset = physical - reinterpret_cast<uintptr_t>(memory->identity);
    const uint64_t virtual_offset = virtual_address - reinterpret_cast<uintptr_t>(memory->module);
    if (physical_offset != virtual_offset || (virtual_offset & (kPageSize - 1u)) != 0u ||
        virtual_offset >= kCapacity) return false;
    const uint64_t page = virtual_offset / kPageSize;
    if (memory->module_flags[page] != starlix::loader::KEXT_RUNTIME_PAGE_UNMAPPED)
        return false;
    int prot = PROT_READ;
    if (flags == 1u) prot |= PROT_WRITE;
    else if (flags == 2u) prot |= PROT_EXEC;
    if (mprotect(memory->module + virtual_offset, kPageSize, prot) != 0)
        return false;
    memory->module_flags[page] = flags;
    ++memory->module_map_calls;
    return true;
}

bool unmap_module_page(void* opaque, uint64_t virtual_address,
                       uint64_t physical, uint32_t flags) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory || !memory->module || !memory->identity ||
        physical < reinterpret_cast<uintptr_t>(memory->identity) ||
        virtual_address < reinterpret_cast<uintptr_t>(memory->module)) return false;
    const uint64_t physical_offset = physical - reinterpret_cast<uintptr_t>(memory->identity);
    const uint64_t virtual_offset = virtual_address - reinterpret_cast<uintptr_t>(memory->module);
    if (physical_offset != virtual_offset || (virtual_offset & (kPageSize - 1u)) != 0u ||
        virtual_offset >= kCapacity) return false;
    const uint64_t page = virtual_offset / kPageSize;
    if (memory->module_flags[page] != flags) return false;
    if (mprotect(memory->module + virtual_offset, kPageSize, PROT_NONE) != 0)
        return false;
    memory->module_flags[page] = starlix::loader::KEXT_RUNTIME_PAGE_UNMAPPED;
    ++memory->module_unmap_calls;
    return true;
}

uint8_t* resolve_write_alias(void* opaque, uint64_t virtual_address,
                             uint64_t size) {
    auto* memory = static_cast<HostMemory*>(opaque);
    if (!memory || !memory->module ||
        virtual_address != reinterpret_cast<uintptr_t>(memory->module) ||
        size != kCapacity) return nullptr;
    return memory->module;
}

bool init_memory(HostMemory& memory) {
    memory.fd = -1;
    for (uint64_t i = 0; i < kPages; ++i)
        memory.module_flags[i] = starlix::loader::KEXT_RUNTIME_PAGE_UNMAPPED;
    char path[] = "/tmp/starlix-kext-runtime-XXXXXX";
    memory.fd = mkstemp(path);
    if (memory.fd < 0) return false;
    unlink(path);
    if (ftruncate(memory.fd, static_cast<off_t>(kCapacity)) != 0) return false;
    void* identity = mmap(nullptr, kCapacity, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_32BIT, memory.fd, 0);
    if (identity == MAP_FAILED ||
        reinterpret_cast<uintptr_t>(identity) >= 0x100000000ULL) return false;
    memory.identity = static_cast<uint8_t*>(identity);
    std::memset(memory.identity, 0, kCapacity);
    for (uint64_t i = 0; i < kPages; ++i)
        memory.identity_writable[i] = 1u;
    void* alias = mmap(nullptr, kCapacity, PROT_NONE, MAP_SHARED, memory.fd, 0);
    if (alias == MAP_FAILED) return false;
    memory.module = static_cast<uint8_t*>(alias);
    return true;
}

void destroy_memory(HostMemory& memory) {
    if (memory.module) munmap(memory.module, kCapacity);
    if (memory.identity) munmap(memory.identity, kCapacity);
    if (memory.fd >= 0) close(memory.fd);
}

struct MockPciResources {
    uint64_t next_token{0xB001u};
    uint32_t claims{};
    uint32_t releases{};
};

bool mock_claim_bar(void* opaque, const StarlixPciDeviceInfoV1* device,
                    StPciU32 bar_index, StarlixPciResourceV1* output) {
    auto* mock = static_cast<MockPciResources*>(opaque);
    if (!mock || !device || !output || bar_index != 0u ||
        !StarlixPciBarIsAssignedMemoryV1(&device->bars[0])) return false;
    ++mock->claims;
    *output = StarlixPciResourceV1{};
    output->struct_size = sizeof(*output);
    output->abi_major = STARLIX_PCI_RESOURCE_ABI_MAJOR;
    output->abi_minor = STARLIX_PCI_RESOURCE_ABI_MINOR;
    output->flags = StarlixPciResourceAssignedV1 |
        StarlixPciResourceMemoryV1 | StarlixPciResourceSizeValidatedV1 |
        StarlixPciResourceClaimedV1;
    output->bus = device->bus;
    output->device = device->device;
    output->function = device->function;
    output->bar_index = static_cast<StPciU8>(bar_index);
    output->base = device->bars[bar_index].base;
    output->size = 0x01000000ULL;
    output->ownership_token = mock->next_token++;
    return true;
}

bool mock_release_resource(void* opaque, StPciU64 token) {
    auto* mock = static_cast<MockPciResources*>(opaque);
    if (!mock || token == 0u) return false;
    ++mock->releases;
    return true;
}

bool read_file(const char* path, uint8_t** bytes, uint64_t* size) {
    if (!path || !bytes || !size) return false;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        static_cast<uint64_t>(st.st_size) > starlix::loader::KEXT_RUNTIME_MAX_PACKAGE_SIZE) {
        close(fd);
        return false;
    }
    auto* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(st.st_size)));
    if (!data) { close(fd); return false; }
    size_t done = 0;
    while (done < static_cast<size_t>(st.st_size)) {
        const ssize_t got = read(fd, data + done, static_cast<size_t>(st.st_size) - done);
        if (got <= 0) { std::free(data); close(fd); return false; }
        done += static_cast<size_t>(got);
    }
    close(fd);
    *bytes = data;
    *size = static_cast<uint64_t>(st.st_size);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <intlgpudvs.sys>\n", argv[0]);
        return 2;
    }
    uint8_t* package = nullptr;
    uint64_t package_size = 0;
    if (!read_file(argv[1], &package, &package_size)) {
        std::fprintf(stderr, "FAIL: could not read external Intel SYS package\n");
        return 2;
    }

    bool ok = true;
    uint8_t* corrupt_package = static_cast<uint8_t*>(std::malloc(package_size));
    if (!corrupt_package) { std::free(package); return 2; }
    std::memcpy(corrupt_package, package, static_cast<size_t>(package_size));
    corrupt_package[package_size - 1u] ^= 0x01u;
    HostMemory rejected_memory{};
    rejected_memory.fd = -1;
    starlix::loader::KEXT_RUNTIME_PLATFORM rejected_platform{};
    rejected_platform.context = &rejected_memory;
    rejected_platform.allocate_pages = allocate_pages;
    rejected_platform.release_pages = release_pages;
    rejected_platform.set_identity_permissions = set_identity_permissions;
    rejected_platform.map_module_page = map_module_page;
    rejected_platform.unmap_module_page = unmap_module_page;
    rejected_platform.guard_unmapped = guard_unmapped;
    rejected_platform.resolve_write_alias = resolve_write_alias;
    starlix::loader::KEXT_RUNTIME_MODULE rejected_module{};
    const auto rejected_status = starlix::loader::KextRuntimeLoadExternal(
        corrupt_package, package_size, 0x700000000000ULL,
        &rejected_platform, &rejected_module);
    ok &= check(rejected_status == starlix::loader::KEXT_RUNTIME_STATUS::PackageRejected &&
                    rejected_memory.allocation_attempts == 0u,
                "corrupt package is rejected before executable-page allocation");
    std::free(corrupt_package);

    HostMemory memory{};
    ok &= init_memory(memory);
    ok &= check(memory.identity != nullptr && memory.module != nullptr,
                "dual physical/module aliases created for bounded KEXT pages");
    if (!ok) { std::free(package); destroy_memory(memory); return 1; }

    starlix::loader::KEXT_RUNTIME_PLATFORM platform{};
    platform.context = &memory;
    platform.allocate_pages = allocate_pages;
    platform.release_pages = release_pages;
    platform.set_identity_permissions = set_identity_permissions;
    platform.map_module_page = map_module_page;
    platform.unmap_module_page = unmap_module_page;
    platform.guard_unmapped = guard_unmapped;
    platform.resolve_write_alias = resolve_write_alias;

    starlix::loader::KEXT_RUNTIME_MODULE module{};
    const uint64_t module_base = reinterpret_cast<uintptr_t>(memory.module);
    const auto status = starlix::loader::KextRuntimeLoadExternal(
        package, package_size, module_base, &platform, &module);
    ok &= check(status == starlix::loader::KEXT_RUNTIME_STATUS::Ready,
                "real external Intel KEXT validates, relocates and maps");
    ok &= check(module.state == 2u && module.entry_address != 0u &&
                    module.applied_relocation_count != 0u,
                "external KEXT remains resident with a resolved entry address");

    uint32_t executable_pages = 0;
    uint32_t writable_pages = 0;
    for (uint64_t i = 0; i < kPages; ++i) {
        if (memory.module_flags[i] == 2u) ++executable_pages;
        if (memory.module_flags[i] == 1u) ++writable_pages;
        ok &= check(!(memory.module_flags[i] == 3u),
                    "no module page is simultaneously writable and executable");
    }
    ok &= check(executable_pages != 0u && writable_pages <= kPages,
                "module has executable pages and bounded writable pages");
    ok &= check(memory.module_map_calls >= kPages &&
                    memory.identity_protect_calls == kPages,
                "identity aliases are read-only before module page activation");

    using Entry = StGpuU64 (*)(const StarlixGpuKextHostApiV1*,
                               StarlixGpuKextDriverApiV1*);
    auto entry = reinterpret_cast<Entry>(static_cast<uintptr_t>(module.entry_address));
    StarlixPciDeviceInfoV1 pci{};
    pci.struct_size = sizeof(pci);
    pci.abi_major = STARLIX_PCI_DEVICE_ABI_MAJOR;
    pci.vendor_id = 0x8086u;
    pci.device_id = 0x0152u;
    pci.class_code = 0x03u;
    pci.subclass = 0x00u;
    pci.flags = StarlixPciDevicePresentV1 | StarlixPciDeviceMemoryDecodeEnabledV1;
    pci.bars[0].base = 0xE0000000u;
    pci.bars[0].raw_low = 0xE0000008u;
    pci.bars[0].flags = StarlixPciBarAssignedV1 | StarlixPciBarMemorySpaceV1 |
        StarlixPciBarPrefetchableV1;

    alignas(16) uint32_t framebuffer[640u * 480u]{};
    alignas(16) uint32_t backbuffer_pixels[640u * 480u]{};
    StarlixGpuHostApi graphics_host{};
    graphics_host.struct_size = sizeof(graphics_host);
    graphics_host.abi_major = 1u;
    graphics_host.pixel_format = StGpuPixelFormatBgrReserved8;
    graphics_host.framebuffer_base = reinterpret_cast<uintptr_t>(framebuffer);
    graphics_host.framebuffer_size = sizeof(framebuffer);
    graphics_host.width = 640u;
    graphics_host.height = 480u;
    graphics_host.pixels_per_scanline = 640u;

    MockPciResources pci_resources_context{};
    StarlixPciResourceHostApiV1 pci_resources{};
    pci_resources.struct_size = sizeof(pci_resources);
    pci_resources.abi_major = STARLIX_PCI_RESOURCE_ABI_MAJOR;
    pci_resources.abi_minor = STARLIX_PCI_RESOURCE_ABI_MINOR;
    pci_resources.context = &pci_resources_context;
    pci_resources.claim_bar = &mock_claim_bar;
    pci_resources.release_resource = &mock_release_resource;

    StarlixGpuKextHostApiV1 host{};
    host.struct_size = sizeof(host);
    host.abi_major = STARLIX_GPU_KEXT_ABI_MAJOR;
    host.abi_minor = STARLIX_GPU_KEXT_ABI_MINOR;
    host.pci_device = &pci;
    host.graphics_host = &graphics_host;
    host.pci_resources = &pci_resources;

    StarlixGpuKextDriverApiV1 driver{};
    driver.struct_size = sizeof(driver);
    const StGpuU64 entry_status = entry(&host, &driver);
    ok &= check(entry_status == StarlixGpuKextSuccessV1,
                "relocated StarlixGpuKextEntry executes and binds Intel HD 2500 ABI");
    ok &= check(driver.abi_major == STARLIX_GPU_KEXT_ABI_MAJOR &&
                    (driver.flags & (StarlixGpuKextPciMatchedV1 |
                                     StarlixGpuKextGopFramebufferReadyV1)) ==
                        (StarlixGpuKextPciMatchedV1 |
                         StarlixGpuKextGopFramebufferReadyV1),
                "executed entry returns PCI match and GOP graphics readiness");
    ok &= check(driver.graphics.fill_rect && driver.graphics.put_pixel &&
                    driver.graphics.present && driver.graphics.animate_rect,
                "executed entry returns callable graphics ABI v2 functions");
    ok &= check(StarlixPciResourceIsValidV1(&driver.bar0_resource) &&
                    driver.bar0_resource.base == 0xE0000000ULL &&
                    driver.bar0_resource.size == 0x01000000ULL &&
                    pci_resources_context.claims == 1u,
                "executed KEXT entry claims the generic sized BAR0 resource");
    const StPciU64 claimed_resource_token =
        driver.bar0_resource.ownership_token;

    StarlixGpuBackbuffer backbuffer{};
    backbuffer.struct_size = sizeof(backbuffer);
    backbuffer.abi_major = 2u;
    backbuffer.pixel_format = StGpuPixelFormatBgrReserved8;
    backbuffer.base = reinterpret_cast<uintptr_t>(backbuffer_pixels);
    backbuffer.size = sizeof(backbuffer_pixels);
    backbuffer.width = 640u;
    backbuffer.height = 480u;
    backbuffer.pixels_per_scanline = 640u;
    const StGpuU64 draw_status = driver.graphics.fill_rect(
        &backbuffer, 10u, 20u, 30u, 40u, 0x00123456u);
    ok &= check(draw_status == StGpuStatusSuccess &&
                    backbuffer_pixels[20u * 640u + 10u] == 0x00123456u &&
                    backbuffer_pixels[59u * 640u + 39u] == 0x00123456u,
                "executed external graphics function renders into caller backbuffer");
    const StGpuU64 present_status = driver.graphics.present(
        &graphics_host, &backbuffer);
    ok &= check(present_status == StGpuStatusSuccess &&
                    framebuffer[20u * 640u + 10u] == 0x00123456u &&
                    framebuffer[59u * 640u + 39u] == 0x00123456u,
                "executed external present function copies backbuffer to GOP framebuffer");

    pci.device_id = 0x0405u;
    driver = StarlixGpuKextDriverApiV1{};
    driver.struct_size = sizeof(driver);
    const StGpuU64 reject_status = entry(&host, &driver);
    ok &= check(reject_status == StarlixGpuKextUnsupportedDeviceV1,
                "same loaded external KEXT rejects a non-target display device");
    ok &= check(pci_resources.release_resource(&pci_resources_context,
                    claimed_resource_token) &&
                    pci_resources_context.releases == 1u,
                "generic PCI resource host API releases the claimed BAR0 token");

    ok &= check(starlix::loader::KextRuntimeUnloadExternal(&module, &platform),
                "external KEXT unload removes aliases, scrubs image and releases pages");
    ok &= check(memory.released && memory.release_calls == 1u && module.state == 0u,
                "unload completes the page lease lifecycle exactly once");
    ok &= check(memory.module_unmap_calls >= kPages,
                "all final module mappings are removed during unload");

    std::free(package);
    destroy_memory(memory);
    std::printf("KEXT_RUNTIME_HOST: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
