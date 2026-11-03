#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

alignas(4096) static uint8_t g_host_staging_pages[64u * 1024u]{};
static bool g_alloc_fail = false;
static bool g_release_fail = false;
static bool g_release_saw_zero = false;
static uint32_t g_alloc_calls = 0u;
static uint32_t g_release_calls = 0u;
static uint64_t g_last_page_count = 0u;

#include "loader/kext_dry_run.hpp"

namespace starlix::mem {
uint64_t managed_alloc_phys_pages(uint64_t page_count) {
    ++g_alloc_calls;
    g_last_page_count = page_count;
    if (g_alloc_fail || page_count != starlix::sys::KEXT_DRY_RUN_PAGE_COUNT)
        return 0ULL;
    std::memset(g_host_staging_pages, 0, sizeof(g_host_staging_pages));
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_host_staging_pages));
}

bool managed_release_phys_pages(uint64_t physical_base, uint64_t page_count) {
    ++g_release_calls;
    const uint64_t expected = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(g_host_staging_pages));
    bool zero = true;
    for (uint8_t byte : g_host_staging_pages) {
        if (byte != 0u) { zero = false; break; }
    }
    g_release_saw_zero = zero;
    return physical_base == expected &&
        page_count == starlix::sys::KEXT_DRY_RUN_PAGE_COUNT &&
        !g_release_fail;
}
}

using namespace starlix::sys;

static int failures = 0;
static void Check(bool condition, const char* name) {
    if (condition) std::printf("PASS: %s\n", name);
    else { std::printf("FAIL: %s\n", name); ++failures; }
}

static std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static void RefreshCrc(std::vector<uint8_t>& package) {
    auto* header = reinterpret_cast<SYS_HEADER*>(package.data());
    header->payload_crc32 = SysPackageCrc32(
        package.data() + header->header_size, header->payload_size);
}

static void ResetHooks() {
    g_alloc_fail = false;
    g_release_fail = false;
    g_release_saw_zero = false;
    g_alloc_calls = 0u;
    g_release_calls = 0u;
    g_last_page_count = 0u;
    std::memset(g_host_staging_pages, 0, sizeof(g_host_staging_pages));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s path/to/intlgpudvs.sys\n", argv[0]);
        return 2;
    }
    const auto package = ReadFile(argv[1]);
    Check(!package.empty(), "live intlgpudvs SYS package read");
    if (package.empty()) return 2;

    ResetHooks();
    KEXT_DRY_RUN_REPORT report{};
    const auto result = KextDryRunPrepareExternalKext(
        package.data(), package.size(), &report);
    Check(result == KEXT_DRY_RUN_STATUS::Ready,
        "external KEXT SYS package accepted by dynamic staging preflight");
    if (result == KEXT_DRY_RUN_STATUS::Ready) {
        Check(report.package_result == SYS_VALIDATE_RESULT::Valid,
            "STSYS001 package validation passed");
        Check(report.relocation_result == SYS_ELF_LOAD_RESULT::Valid,
            "ELF64 relocation completed without executing entry");
        Check(report.image_size > 0u &&
            report.image_size <= KEXT_DRY_RUN_IMAGE_CAPACITY,
            "relocated image remains within leased page capacity");
        Check(report.image_alignment >= 16u &&
            (report.image_alignment & (report.image_alignment - 1u)) == 0u,
            "relocated image alignment is valid for alloc sections");
        Check(report.loaded_section_count >= 2u,
            "relocator accepted executable and compiler-generated alloc sections");
        Check(report.applied_relocation_count >= 27u,
            "all supported executable relocations were applied");
        Check(report.entry_offset < report.image_size,
            "packaged KEXT entry resolves inside staged image");
        Check(report.staging_page_count == KEXT_DRY_RUN_PAGE_COUNT &&
              g_last_page_count == KEXT_DRY_RUN_PAGE_COUNT,
            "temporary image uses the exact bounded managed-page lease");
        Check(report.buffer_scrubbed == 1u && g_release_saw_zero,
            "temporary relocated code buffer is fully scrubbed before release");
        Check(report.pages_released == 1u && g_release_calls == 1u,
            "temporary managed-page lease is returned after preflight");
    }

    ResetHooks();
    auto wrongTarget = package;
    auto* wrongHeader = reinterpret_cast<SYS_HEADER*>(wrongTarget.data());
    std::memset(wrongHeader->module_name, 0, sizeof(wrongHeader->module_name));
    std::memcpy(wrongHeader->module_name, "notagpudriver", 13);
    RefreshCrc(wrongTarget);
    KEXT_DRY_RUN_REPORT wrongReport{};
    Check(KextDryRunPrepareExternalKext(wrongTarget.data(), wrongTarget.size(),
            &wrongReport) == KEXT_DRY_RUN_STATUS::Ready,
        "generic KEXT loader accepts a valid external module name");
    Check(g_alloc_calls == 1u && g_release_calls == 1u &&
          wrongReport.buffer_scrubbed == 1u && wrongReport.pages_released == 1u,
        "generic KEXT staging does not special-case Intel module identity");

    ResetHooks();
    std::vector<uint8_t> corrupt = package;
    corrupt.back() ^= 0x01u;
    KEXT_DRY_RUN_REPORT badReport{};
    Check(KextDryRunPrepareExternalKext(corrupt.data(), corrupt.size(),
            &badReport) == KEXT_DRY_RUN_STATUS::PackageRejected,
        "corrupt SYS CRC rejected before page allocation");
    Check(g_alloc_calls == 0u,
        "invalid package is rejected before allocating staging pages");

    ResetHooks();
    auto badElf = package;
    auto* badHeader = reinterpret_cast<SYS_HEADER*>(badElf.data());
    auto* elf = reinterpret_cast<SYS_ELF64_HEADER*>(
        badElf.data() + badHeader->header_size);
    elf->shnum = 0u;
    RefreshCrc(badElf);
    KEXT_DRY_RUN_REPORT badElfReport{};
    Check(KextDryRunPrepareExternalKext(badElf.data(), badElf.size(),
            &badElfReport) == KEXT_DRY_RUN_STATUS::PackageRejected,
        "malformed ELF metadata is rejected by package validation");
    Check(g_alloc_calls == 0u && g_release_calls == 0u,
        "malformed ELF is rejected before allocating staging pages");

    ResetHooks();
    g_alloc_fail = true;
    Check(KextDryRunPrepareExternalKext(package.data(), package.size(),
            &badReport) == KEXT_DRY_RUN_STATUS::MemoryUnavailable,
        "staging fails closed when managed memory is unavailable");
    Check(g_release_calls == 0u,
        "failed allocation does not attempt to release an absent lease");

    ResetHooks();
    g_release_fail = true;
    Check(KextDryRunPrepareExternalKext(package.data(), package.size(),
            &badReport) == KEXT_DRY_RUN_STATUS::ReleaseFailed,
        "page-lease release failure is reported");
    Check(badReport.buffer_scrubbed == 1u &&
          badReport.pages_released == 0u && g_release_saw_zero,
        "release failure is reported only after staging bytes are scrubbed");

    Check(KextDryRunPrepareExternalKext(package.data(), package.size(), nullptr) ==
            KEXT_DRY_RUN_STATUS::InvalidArgument,
        "missing report pointer rejected");
    Check(KextDryRunPrepareExternalKext(package.data(),
            KEXT_DRY_RUN_MAX_PACKAGE_SIZE + 1, &badReport) ==
            KEXT_DRY_RUN_STATUS::InvalidArgument,
        "oversized staged package rejected before reading");

    if (failures) {
        std::printf("KEXT_DYNAMIC_STAGE_HOST: FAIL (%d failures)\n", failures);
        return 1;
    }
    std::printf("KEXT_DYNAMIC_STAGE_HOST: PASS (%u relocations; buffer scrubbed; no entry execution)\n",
        report.applied_relocation_count);
    return 0;
}
