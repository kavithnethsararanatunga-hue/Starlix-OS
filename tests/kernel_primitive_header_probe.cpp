// Mirrors the primitive type prelude in kernel/src/main.cpp. This translation
// unit is intentionally freestanding and is compiled without -I<repo>/include.
#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

#include "kernel/src/loader/kext_dry_run.hpp"

static_assert(sizeof(starlix::sys::SYS_HEADER) == 104);
static_assert(starlix::sys::KEXT_DRY_RUN_IMAGE_CAPACITY == 64ULL * 1024ULL);
static_assert(starlix::sys::KEXT_DRY_RUN_MAX_PACKAGE_SIZE == 1024ULL * 1024ULL);

int KernelPrimitiveHeaderProbe() {
    starlix::sys::KEXT_DRY_RUN_REPORT report{};
    return static_cast<int>(report.image_size);
}
