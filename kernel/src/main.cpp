// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

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

#include "bootinfo.hpp"
#include "rxfs_native.hpp"
#include "cpu/gdt.hpp"
#include "cpu/fpu.hpp"
#include "cpu/idt.hpp"
#include "cpu/syscall.hpp"
#include "ob/object.hpp"
#include "mem/vmm.hpp"
#include "mem/address_space.hpp"
#include "mem/managed_page_pool.hpp"
#include "mem/phys.hpp"
#include "mem/kernel_stack.hpp"
#include "process/process.hpp"
#include "process/scheduler.hpp"
#include "process/context_switch.hpp"
#include "process/user_mode_smoke_test.hpp"
#include "ipc/alpc.hpp"
#include "loader/ect_loader.hpp"
#include "loader/kext_runtime.hpp"
#include "../../include/driver/gpu_kext_abi.hpp"
#include "loader/system_component.hpp"
#include "gui/window.hpp"
#include "drivers/pci.hpp"
#include "drivers/pci_resource.hpp"
#include "drivers/ehci.hpp"
#include "drivers/uhci.hpp"
#include "drivers/framebuffer.hpp"
#include "drivers/serial.hpp"
#include "storage/storage.hpp"

static StarlixBootInfo* g_BootInfo = nullptr;
static starlix::loader::KEXT_RUNTIME_MODULE g_external_gpu_kext{};
static StarlixGpuKextDriverApiV1 g_external_gpu_driver_api{};
static StarlixPciDeviceInfoV1 g_external_gpu_pci_device{};
static StarlixGpuHostApi g_external_gpu_graphics_host{};
static StarlixPciResourceHostApiV1 g_external_gpu_pci_resource_host{};
static bool g_external_gpu_driver_ready = false;
static uint32_t usb_input_diagnostic_events = 0u;
static uint8_t usb_input_last_mouse_buttons = 0u;
static bool usb_input_have_mouse_buttons = false;

static void DebugLog(const char* msg, uint32_t color = 0x004ADE80) {
    (void)color;
    if (!msg) return;
    starlix::drivers::serial::print(msg);
    starlix::drivers::serial::print("\r\n");
}

static void serial_print_u32(uint32_t value) {
    char digits[10];
    uint32_t count = 0u;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u) {
        const char digit[2] = {digits[--count], '\0'};
        starlix::drivers::serial::print(digit);
    }
}

static void serial_print_u8_hex(uint8_t value) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    const char digits[3] = {HEX[(value >> 4u) & 0x0Fu], HEX[value & 0x0Fu], '\0'};
    starlix::drivers::serial::print(digits);
}

static void serial_print_i16(int16_t value) {
    const int32_t wide = value;
    if (wide < 0) {
        starlix::drivers::serial::print("-");
        serial_print_u32(static_cast<uint32_t>(-wide));
    } else {
        serial_print_u32(static_cast<uint32_t>(wide));
    }
}

struct GpuKextProbeContext {
    StarlixGpuKextDriverApiV1 driver_api;
    StarlixPciDeviceInfoV1 bound_device;
    uint32_t entry_calls;
    uint64_t last_status;
    bool bound;
};

using GpuKextEntryFn = decltype(&StarlixGpuKextEntry);

static bool gpu_kext_claim_pci_bar(
    void*, const StarlixPciDeviceInfoV1* device,
    StPciU32 bar_index, StarlixPciResourceV1* output) {
    if (!device || !output) return false;
    return starlix::drivers::pci::pci_claim_bar_resource(
        *device, bar_index, *output);
}

static bool gpu_kext_release_pci_resource(void*, StPciU64 token) {
    return starlix::drivers::pci::pci_release_bar_resource(token);
}

static bool gpu_kext_pci_visitor(
    const StarlixPciDeviceInfoV1& device, void* opaque) {
    auto* context = static_cast<GpuKextProbeContext*>(opaque);
    if (!context || context->bound || device.class_code != 0x03u) return true;

    const auto entry = reinterpret_cast<GpuKextEntryFn>(
        static_cast<uintptr_t>(g_external_gpu_kext.entry_address));
    if (!entry) return true;

    StarlixGpuKextHostApiV1 host{};
    host.struct_size = sizeof(host);
    host.abi_major = STARLIX_GPU_KEXT_ABI_MAJOR;
    host.abi_minor = STARLIX_GPU_KEXT_ABI_MINOR;
    host.pci_device = &device;
    host.graphics_host = &g_external_gpu_graphics_host;
    host.pci_resources = &g_external_gpu_pci_resource_host;

    StarlixGpuKextDriverApiV1 driver_api{};
    driver_api.struct_size = sizeof(driver_api);
    const StGpuU64 status = entry(&host, &driver_api);
    ++context->entry_calls;
    context->last_status = status;

    if (status != StarlixGpuKextSuccessV1 ||
        driver_api.abi_major != STARLIX_GPU_KEXT_ABI_MAJOR ||
        (driver_api.flags & (StarlixGpuKextPciMatchedV1 |
                             StarlixGpuKextGopFramebufferReadyV1)) !=
            (StarlixGpuKextPciMatchedV1 |
             StarlixGpuKextGopFramebufferReadyV1) ||
        driver_api.graphics.abi_major != 2u ||
        driver_api.struct_size < sizeof(StarlixGpuKextDriverApiV1) ||
        driver_api.graphics.struct_size < sizeof(StarlixGpuFrameDriverApi) ||
        (driver_api.capabilities & StarlixGpuKextCapabilityGopFramebufferV1) == 0ULL ||
        !driver_api.graphics.put_pixel ||
        !driver_api.graphics.fill_rect ||
        !driver_api.graphics.clear ||
        !driver_api.graphics.present ||
        !driver_api.graphics.animate_rect ||
        !StarlixPciResourceIsValidV1(&driver_api.bar0_resource)) {
        if (driver_api.bar0_resource.ownership_token != 0ULL)
            (void)gpu_kext_release_pci_resource(
                nullptr, driver_api.bar0_resource.ownership_token);
        return true;
    }

    context->driver_api = driver_api;
    context->bound_device = device;
    context->bound = true;
    return false;
}

static bool gpu_kext_package_entry_matches(
    const uint8_t* package, uint64_t package_size) {
    if (!package || package_size < sizeof(starlix::sys::SYS_HEADER)) return false;
    const auto* header = reinterpret_cast<const starlix::sys::SYS_HEADER*>(package);
    static constexpr char expected[] = "StarlixGpuKextEntry";
    constexpr uint32_t expected_length = sizeof(expected) - 1u;
    if (header->header_size != sizeof(starlix::sys::SYS_HEADER) ||
        header->payload_size != package_size - header->header_size ||
        sizeof(header->entry_symbol) <= expected_length) return false;
    for (uint32_t i = 0u; i < expected_length; ++i)
        if (header->entry_symbol[i] != expected[i]) return false;
    if (header->entry_symbol[expected_length] != '\0') return false;
    return true;
}

static bool load_external_gpu_kext(StarlixBootInfo* boot_info,
                                   bool& pci_scan_performed) {
    pci_scan_performed = false;
    g_external_gpu_driver_ready = false;
    g_external_gpu_kext = starlix::loader::KEXT_RUNTIME_MODULE{};
    g_external_gpu_driver_api = StarlixGpuKextDriverApiV1{};
    g_external_gpu_pci_device = StarlixPciDeviceInfoV1{};
    g_external_gpu_graphics_host = StarlixGpuHostApi{};

    if (!boot_info ||
        !starlix::mem::address_space_manager_ready() ||
        !starlix::mem::managed_page_pool_ready() ||
        !starlix::drivers::framebuffer::ready() ||
        boot_info->SystemModuleDescriptorSize != sizeof(StarlixBootModule) ||
        boot_info->SystemModuleCount == 0u ||
        boot_info->SystemModuleCount > 256u ||
        boot_info->SystemModuleTableBase == 0ULL) {
        starlix::drivers::serial::print(
            "[KEXT] runtime load unavailable; prerequisite check failed.\r\n");
        return false;
    }

    const auto* modules = reinterpret_cast<const StarlixBootModule*>(
        static_cast<uintptr_t>(boot_info->SystemModuleTableBase));
    const uint8_t* package = nullptr;
    uint64_t package_size = 0ULL;
    const char* package_path = nullptr;
    for (uint32_t i = 0u; i < boot_info->SystemModuleCount; ++i) {
        const StarlixBootModule& module = modules[i];
        if (module.Kind != StarlixBootModuleKindKextDb || module.Flags != 1u ||
            module.ImageBase == 0ULL ||
            module.ImageSize < sizeof(starlix::sys::SYS_HEADER) ||
            module.ImageSize > starlix::loader::KEXT_RUNTIME_MAX_PACKAGE_SIZE ||
            module.ImageBase > (~0ULL - module.ImageSize)) continue;

        bool path_terminated = false;
        for (uint32_t n = 0u; n < sizeof(module.Path); ++n) {
            if (module.Path[n] == '\0') { path_terminated = true; break; }
        }
        if (!path_terminated) continue;
        const auto* candidate = reinterpret_cast<const uint8_t*>(
            static_cast<uintptr_t>(module.ImageBase));
        if (!gpu_kext_package_entry_matches(candidate, module.ImageSize)) continue;
        package = candidate;
        package_size = module.ImageSize;
        package_path = module.Path;
        break;
    }

    if (!package) {
        starlix::drivers::serial::print(
            "[KEXT] no staged module with the supported generic GPU entry ABI.\r\n");
        return false;
    }

    starlix::loader::KEXT_RUNTIME_PLATFORM platform{};
    starlix::loader::KextRuntimeMakeKernelPlatform(platform);
    const uint64_t module_virtual_base =
        starlix::mem::KERNEL_MODULE_REGION_BASE +
        starlix::loader::KEXT_RUNTIME_PAGE_SIZE;
    const auto load_status = starlix::loader::KextRuntimeLoadExternal(
        package, package_size, module_virtual_base, &platform,
        &g_external_gpu_kext);
    if (load_status != starlix::loader::KEXT_RUNTIME_STATUS::Ready) {
        starlix::drivers::serial::print("[KEXT] external module load failed; status=");
        serial_print_u32(static_cast<uint32_t>(load_status));
        starlix::drivers::serial::print(".\r\n");
        return false;
    }

    starlix::drivers::serial::print("[KEXT] external module mapped: ");
    starlix::drivers::serial::print(package_path);
    starlix::drivers::serial::print("; image=");
    serial_print_u32(static_cast<uint32_t>(g_external_gpu_kext.image_size));
    starlix::drivers::serial::print("; sections=");
    serial_print_u32(g_external_gpu_kext.loaded_section_count);
    starlix::drivers::serial::print("; relocations=");
    serial_print_u32(g_external_gpu_kext.applied_relocation_count);
    starlix::drivers::serial::print("; W^X page mappings verified.\r\n");

    g_external_gpu_graphics_host.struct_size = sizeof(StarlixGpuHostApi);
    g_external_gpu_graphics_host.abi_major = 1u;
    g_external_gpu_graphics_host.abi_minor = 0u;
    g_external_gpu_graphics_host.pixel_format = boot_info->FramebufferPixelFormat;
    g_external_gpu_graphics_host.framebuffer_base = boot_info->FramebufferBase;
    g_external_gpu_graphics_host.framebuffer_size = boot_info->FramebufferSize;
    g_external_gpu_graphics_host.width = boot_info->FramebufferWidth;
    g_external_gpu_graphics_host.height = boot_info->FramebufferHeight;
    g_external_gpu_graphics_host.pixels_per_scanline =
        boot_info->FramebufferPixelsPerScanLine;

    g_external_gpu_pci_resource_host = StarlixPciResourceHostApiV1{};
    g_external_gpu_pci_resource_host.struct_size =
        sizeof(StarlixPciResourceHostApiV1);
    g_external_gpu_pci_resource_host.abi_major =
        STARLIX_PCI_RESOURCE_ABI_MAJOR;
    g_external_gpu_pci_resource_host.abi_minor =
        STARLIX_PCI_RESOURCE_ABI_MINOR;
    g_external_gpu_pci_resource_host.claim_bar = &gpu_kext_claim_pci_bar;
    g_external_gpu_pci_resource_host.release_resource =
        &gpu_kext_release_pci_resource;

    GpuKextProbeContext probe{};

    pci_scan_performed = true;
    (void)starlix::drivers::pci::pci_scan_all(
        &gpu_kext_pci_visitor, &probe);

    if (!probe.bound) {
        starlix::drivers::serial::print("[KEXT] entry calls=");
        serial_print_u32(probe.entry_calls);
        if (probe.entry_calls != 0u) {
            starlix::drivers::serial::print("; last entry status=");
            serial_print_u32(static_cast<uint32_t>(probe.last_status));
        }
        starlix::drivers::serial::print("; no compatible display device; unloading module.\r\n");
        if (!starlix::loader::KextRuntimeUnloadExternal(
                &g_external_gpu_kext, &platform)) {
            starlix::drivers::serial::print(
                "[KEXT] unload rollback incomplete; memory remains quarantined.\r\n");
        } else {
            starlix::drivers::serial::print(
                "[KEXT] module unmapped, scrubbed and released.\r\n");
        }
        return false;
    }

    g_external_gpu_driver_api = probe.driver_api;
    g_external_gpu_pci_device = probe.bound_device;
    g_external_gpu_driver_ready = true;
    starlix::drivers::serial::print(
        "[KEXT] StarlixGpuKextEntry EXECUTED; status=SUCCESS; GOP framebuffer backend ready.\r\n");
    starlix::drivers::serial::print("[KEXT] PCI identity ");
    serial_print_u8_hex(g_external_gpu_pci_device.bus);
    starlix::drivers::serial::print(":");
    serial_print_u8_hex(g_external_gpu_pci_device.device);
    starlix::drivers::serial::print(".");
    serial_print_u8_hex(g_external_gpu_pci_device.function);
    starlix::drivers::serial::print(" vendor=");
    serial_print_u32(g_external_gpu_pci_device.vendor_id);
    starlix::drivers::serial::print(" device=");
    serial_print_u32(g_external_gpu_pci_device.device_id);
    starlix::drivers::serial::print("; BAR0 base=");
    serial_print_u32(static_cast<uint32_t>(
        g_external_gpu_driver_api.bar0_resource.base >> 32u));
    starlix::drivers::serial::print(":");
    serial_print_u32(static_cast<uint32_t>(
        g_external_gpu_driver_api.bar0_resource.base & 0xFFFFFFFFu));
    starlix::drivers::serial::print(" size=");
    serial_print_u32(static_cast<uint32_t>(
        g_external_gpu_driver_api.bar0_resource.size));
    starlix::drivers::serial::print("; BAR0 exclusively claimed; MMIO not mapped; native acceleration disabled.\r\n");
    return true;
}

/* Print event metadata, never key text. The first 16 events are individually
   visible for bring-up; afterward one event per 64 is logged to cap serial I/O. */
static void serial_log_usb_input_event(
        const starlix::drivers::ehci::EHCIInputEvent& event) {
    if (usb_input_diagnostic_events != 0xFFFFFFFFu)
        ++usb_input_diagnostic_events;
    const bool keyboard_transition = event.type ==
        starlix::drivers::ehci::hid_input::EventType::KeyDown || event.type ==
        starlix::drivers::ehci::hid_input::EventType::KeyUp;
    const bool mouse_button_transition = event.type ==
        starlix::drivers::ehci::hid_input::EventType::MouseReport &&
        (!usb_input_have_mouse_buttons || event.buttons != usb_input_last_mouse_buttons);
    const bool wheel_event = event.type ==
        starlix::drivers::ehci::hid_input::EventType::MouseReport && event.wheel != 0;
    const bool movement_sample = usb_input_diagnostic_events <= 16u ||
        (usb_input_diagnostic_events & 63u) == 0u;
    if (!keyboard_transition && !mouse_button_transition && !wheel_event && !movement_sample) return;
    if (event.type == starlix::drivers::ehci::hid_input::EventType::MouseReport) {
        usb_input_last_mouse_buttons = event.buttons;
        usb_input_have_mouse_buttons = true;
    }
    starlix::drivers::serial::print("[USB-INPUT] delivered #");
    serial_print_u32(usb_input_diagnostic_events);
    starlix::drivers::serial::print(" type=");
    serial_print_u32(static_cast<uint8_t>(event.type));
    starlix::drivers::serial::print(" usage=0x");
    serial_print_u8_hex(event.code);
    starlix::drivers::serial::print(" modifiers=0x");
    serial_print_u8_hex(event.modifiers);
    starlix::drivers::serial::print(" buttons=0x");
    serial_print_u8_hex(event.buttons);
    starlix::drivers::serial::print(" dx=");
    serial_print_i16(event.dx);
    starlix::drivers::serial::print(" dy=");
    serial_print_i16(event.dy);
    starlix::drivers::serial::print(" wheel=");
    serial_print_i16(event.wheel);
    starlix::drivers::serial::print("\n");
}

/* Temporary EHCI/DMA diagnostic: report the first point where an allocator
   which initialized successfully stops reporting ready. Serial-only to avoid
   adding more framebuffer work during early kernel initialization. */
static void CheckDmaAllocatorState(bool initialized, bool& loss_reported,
                                   const char* stage) {
    if (!initialized || loss_reported || starlix::mem::phys_allocator_ready()) return;
    starlix::drivers::serial::print("[DMA-CHECK] DMA32 allocator lost ready state after ");
    starlix::drivers::serial::print(stage);
    starlix::drivers::serial::print("\r\n");
    loss_reported = true;
}

extern "C" void KernelPrint(const char* msg, int y_offset) {
    (void)y_offset;
    DebugLog(msg, 0x00FBBF24);
}

extern "C" unsigned char __starlix_boot_stack_bottom;
extern "C" unsigned char __starlix_boot_stack_top;
extern "C" uint64_t StNtSystemCallHandler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

extern "C" void KernelMain(StarlixBootInfo* bootInfo) {
    if (bootInfo && bootInfo->FramebufferBase) {
        g_BootInfo = bootInfo;
        starlix::drivers::serial::init();
        if (starlix::drivers::framebuffer::init(bootInfo))
            starlix::drivers::serial::print("[DISPLAY] GOP framebuffer transport=READY\r\n");
        else
            starlix::drivers::serial::print("[DISPLAY] GOP framebuffer transport=UNAVAILABLE\r\n");
        starlix::gui::gui_init(bootInfo);

        if (bootInfo &&
            bootInfo->SystemModuleDescriptorSize == sizeof(StarlixBootModule) &&
            bootInfo->SystemModuleCount <= 256 && // STARLIX_PHASE13A_KEXT_TABLE_LIMIT
            (bootInfo->SystemModuleCount == 0 ||
             bootInfo->SystemModuleTableBase != 0)) {
            starlix::drivers::serial::print(
                "[MODULE] External payload handoff table validated; "
                "modules are staged for deferred runtime loading.\r\n");

            const auto* modules =
                reinterpret_cast<const StarlixBootModule*>(
                    static_cast<uintptr_t>(bootInfo->SystemModuleTableBase));

            for (uint32_t i = 0; i < bootInfo->SystemModuleCount; ++i) {
                const StarlixBootModule* module = &modules[i];
                if (!module->ImageBase || !module->ImageSize ||
                    module->ImageSize > 64ULL * 1024ULL * 1024ULL ||
                    module->ImageBase > (~0ULL - module->ImageSize))
                    continue;

                bool nameTerminated = false;
                for (uint32_t n = 0; n < sizeof(module->Path); ++n) {
                    if (module->Path[n] == '\0') {
                        nameTerminated = true;
                        break;
                    }
                }
                if (!nameTerminated) continue;

                starlix::drivers::serial::print("[MODULE] Staged: ");
                starlix::drivers::serial::print(module->Path);
                if (module->Kind == StarlixBootModuleKindDriverSys) {
                    starlix::drivers::serial::print(" [SYS; not executed]\r\n");
                } else if (module->Kind == StarlixBootModuleKindLibraryLbr) {
                    starlix::drivers::serial::print(" [LBR; not executed]\r\n");
                } else if (module->Kind == StarlixBootModuleKindKextDb) {
                    starlix::drivers::serial::print(
                        " [KEXT DB payload staged; runtime load deferred until memory services are ready]\r\n");
                } else {
                    starlix::drivers::serial::print(" [unknown kind]\r\n");
                }
            }
        } else {
            starlix::drivers::serial::print(
                "[MODULE] No valid external module handoff table.\r\n");
        }


        DebugLog("==============================================", 0x0038BDF8);
        DebugLog(" STARLIX OS KERNEL - HARDWARE BOOT SEQUENCE   ", 0x0038BDF8);
        DebugLog("==============================================", 0x0038BDF8);

        // 1. Initialize GDT & TSS
        starlix::cpu::gdt_init();

        if (!starlix::cpu::tss_exception_stacks_ready()) {
            starlix::drivers::serial::print(
                "[TSS] FATAL: exception IST stack verification failed.\r\n");
            for (;;) asm volatile("cli; hlt");
        }

        starlix::drivers::serial::print(
            "[TSS] IST1-IST4 exception stacks ready; I/O bitmap disabled.\r\n");

        const uint64_t bootstrap_stack_bottom =
            reinterpret_cast<uint64_t>(&__starlix_boot_stack_bottom);
        const uint64_t bootstrap_stack_top =
            reinterpret_cast<uint64_t>(&__starlix_boot_stack_top);

        uint64_t current_stack_pointer = 0;
        asm volatile("mov %%rsp, %0" : "=r"(current_stack_pointer));

        if (bootstrap_stack_top <= bootstrap_stack_bottom ||
            (bootstrap_stack_bottom & 4095ULL) != 0 ||
            (bootstrap_stack_top & 15ULL) != 0 ||
            bootstrap_stack_top - bootstrap_stack_bottom != 65536ULL ||
            current_stack_pointer < bootstrap_stack_bottom ||
            current_stack_pointer >= bootstrap_stack_top) {
            starlix::drivers::serial::print(
                "[STACK] FATAL: bootstrap stack verification failed.\r\n");
            for (;;) asm volatile("cli; hlt");
        }

        if (!starlix::cpu::tss_set_kernel_stack_checked(bootstrap_stack_top)) {
            starlix::drivers::serial::print(
                "[STACK] FATAL: TSS RSP0 rejected the bootstrap stack.\r\n");
            for (;;) asm volatile("cli; hlt");
        }

        starlix::drivers::serial::print(
            "[STACK] Kernel-owned 64 KiB stack verified; TSS RSP0 configured.\r\n");

        DebugLog("[OK] Global Descriptor Table (GDT) Loaded");

        // 2. Initialize IDT
        starlix::cpu::idt_init();

        if (!starlix::cpu::idt_exception_ist_ready()) {
            starlix::drivers::serial::print(
                "[IDT] FATAL: exception IST gate verification failed.\r\n");
            for (;;) asm volatile("cli; hlt");
        }

        starlix::drivers::serial::print(
            "[IDT] Exception vectors 0-31 installed with IST1-IST4.\r\n");
        DebugLog("[OK] Interrupt Descriptor Table (IDT) Loaded");
        const bool nxe_ready = starlix::mem::vmm_enable_nxe();
        starlix::drivers::serial::print(nxe_ready
            ? "[VMM] NX_SUPPORT=1\r\n[VMM] NXE_ENABLE=PASS\r\n"
            : "[VMM] NXE_ENABLE=FAIL\r\n");
        if (!nxe_ready) {
            starlix::drivers::serial::print("[VMM] NXE_REASON=");
            starlix::drivers::serial::print(starlix::mem::vmm_nxe_failure_reason());
            starlix::drivers::serial::print("\r\n");
        }
        if (starlix::cpu::timer_init(1000u)) {
            starlix::drivers::serial::print("[OK] PIT monotonic timer initialized; IRQ0 tick observed (1000-Hz target).\n");
        } else {
            starlix::drivers::serial::print("[WARN] PIT timer unavailable; timer-backed EHCI requests will be rejected.\n");
        }

        // 3. Initialize Fast Syscall Engine
        if (starlix::cpu::syscall_init()) {
            DebugLog("[OK] SYSCALL entry configured; Ring 3 remains disabled",
                     0x0038BDF8);
        } else {
            starlix::drivers::serial::print(
                "[WARN] SYSCALL initialization failed; Ring 3 remains disabled.\\r\\n");
        }

        // 4. Initialize Executive Objects
        starlix::ob::ob_init();
        const bool dma_allocator_initialized = starlix::mem::phys_init(bootInfo);
        bool dma_allocator_loss_reported = false;
        if (dma_allocator_initialized) {
            const uint64_t dma_probe_page = starlix::mem::alloc_phys_page();
            if (dma_probe_page != 0 && (dma_probe_page & 4095ULL) == 0 &&
                dma_probe_page <= 0xFFFFF000ULL && starlix::mem::phys_allocator_ready()) {
                DebugLog("[OK] UEFI map-backed DMA32 page allocator self-test passed");
                // Phase 3A: contiguous physical-page allocation boot test.
                // The allocation is intentionally boot-lifetime; no free API exists.
                constexpr uint64_t contiguous_probe_bytes = 3ULL * 4096ULL;
                const uint64_t contiguous_probe =
                    starlix::mem::alloc_phys_pages(3ULL);

                bool contiguous_probe_ok =
                    contiguous_probe != 0 &&
                    (contiguous_probe & 4095ULL) == 0 &&
                    contiguous_probe <=
                        (0x100000000ULL - contiguous_probe_bytes) &&
                    starlix::mem::phys_allocator_ready();

                if (contiguous_probe_ok) {
                    const volatile uint8_t* memory =
                        reinterpret_cast<const volatile uint8_t*>(
                            static_cast<uintptr_t>(contiguous_probe));

                    // Confirm that every byte in the newly allocated range
                    // was zeroed by the allocator.
                    for (uint64_t offset = 0;
                         offset < contiguous_probe_bytes; ++offset) {
                        if (memory[offset] != 0) {
                            contiguous_probe_ok = false;
                            break;
                        }
                    }
                }

                if (contiguous_probe_ok) {
                    volatile uint8_t* memory =
                        reinterpret_cast<volatile uint8_t*>(
                            static_cast<uintptr_t>(contiguous_probe));

                    const uint8_t markers[3] = {0xA5u, 0x5Au, 0xC3u};

                    // Touch the beginning of each allocated page.
                    for (uint32_t page = 0; page < 3; ++page)
                        memory[static_cast<uint64_t>(page) * 4096ULL] =
                            markers[page];

                    for (uint32_t page = 0; page < 3; ++page) {
                        if (memory[static_cast<uint64_t>(page) * 4096ULL] !=
                            markers[page]) {
                            contiguous_probe_ok = false;
                            break;
                        }
                    }

                    // Leave the test locations in their original zero state.
                    for (uint32_t page = 0; page < 3; ++page)
                        memory[static_cast<uint64_t>(page) * 4096ULL] = 0;
                }

                if (contiguous_probe_ok) {
                    starlix::drivers::serial::print(
                        "[MEM] Contiguous 3-page allocation passed: "
                        "12 KiB, aligned, zeroed and writable.\r\n");
                } else {
                    starlix::drivers::serial::print(
                        "[MEM] WARNING: contiguous 3-page allocation "
                        "self-test failed.\r\n");
                }

            } else {
                DebugLog("[WARN] DMA32 allocator page probe failed or allocator state changed", 0x00FBBF24);
                starlix::drivers::serial::print("[DMA-CHECK] Allocator invariant failed immediately after page probe.\n");
            }
        } else {
            DebugLog("[WARN] Physical allocator unavailable; static EHCI DMA guards remain active", 0x00FBBF24);
        }
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "phys_init / first-page self-test");
        starlix::mem::vmm_init();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "vmm_init");
        const bool managed_page_pool_initialized =
            starlix::mem::address_space_manager_ready() &&
            starlix::mem::managed_page_pool_init();
        starlix::drivers::serial::print(managed_page_pool_initialized
            ? "[MEM] MANAGED_PAGE_POOL=PASS\r\n"
            : "[MEM] MANAGED_PAGE_POOL=FAIL; PREREQUISITE_UNAVAILABLE\r\n");

        // External KEXT execution is deferred until the runtime loader below,
        // after memory and page-table services have initialized.
        const bool kernel_stack_manager_initialized =
            starlix::mem::address_space_manager_ready() &&
            managed_page_pool_initialized &&
            starlix::mem::kernel_stack_init();
        if (kernel_stack_manager_initialized) {
            starlix::drivers::serial::print(
                "[STACK] Shared high-half supervisor/NX stack arena ready; guard-page layout reserved.\r\n");
        } else {
            starlix::drivers::serial::print(
                "[STACK] WARNING: guarded kernel-stack arena unavailable; user-thread switching stays disabled.\r\n");
        }
        starlix::process::process_init();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "process_init");
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "kernel_stack_init");
        const bool scheduler_prerequisites =
            starlix::mem::address_space_manager_ready() &&
            managed_page_pool_initialized &&
            kernel_stack_manager_initialized;
        if (scheduler_prerequisites)
            starlix::process::scheduler::scheduler_init();
        const bool scheduler_initialized = scheduler_prerequisites &&
            starlix::process::scheduler::scheduler_ready();
        starlix::drivers::serial::print(scheduler_initialized
            ? "[SCHEDULER] READY=PASS\r\n"
            : "[SCHEDULER] READY=FAIL; PREREQUISITE_UNAVAILABLE\r\n");
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "scheduler_init");
        uint64_t fpu_saved_flags = 0ULL;
        asm volatile("pushfq; popq %0; cli" : "=r"(fpu_saved_flags) :: "memory");
        const bool fpu_initialized = starlix::cpu::fpu_init();
        if ((fpu_saved_flags & (1ULL << 9)) != 0ULL)
            asm volatile("sti" ::: "memory");
        starlix::drivers::serial::print(fpu_initialized
            ? "[CPU] FPU_SSE_CONTEXT=PASS\r\n"
            : "[CPU] FPU_SSE_CONTEXT=FAIL; USER_SWITCHING_DISABLED\r\n");
        if (scheduler_initialized && starlix::cpu::fpu_ready())
            starlix::process::context_switch::context_switch_init();
        starlix::drivers::serial::print(
            starlix::process::context_switch::context_switch_ready()
                ? "[CONTEXT] COORDINATOR=PASS\r\n"
                : "[CONTEXT] COORDINATOR=FAIL; PREREQUISITE_UNAVAILABLE\r\n");
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "context_switch_init");
        starlix::ipc::alpc_init();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "alpc_init");
        starlix::loader::loader_init();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "loader_init");
        DebugLog("[OK] Starlix Executive Subsystems (OB, VMM, ALPC) Active");

        // 5. Load an external module only after physical memory, managed pages,
        // address-space mappings and GOP services are ready. PCI enumeration
        // remains generic; device matching is performed inside the KEXT.
        bool gpu_pci_scan_performed = false;
        if (!load_external_gpu_kext(bootInfo, gpu_pci_scan_performed) &&
            !gpu_pci_scan_performed)
            (void)starlix::drivers::pci::pci_scan_all();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "external KEXT load / pci_scan_all");

        // 6. Initialize USB EHCI Hardware Driver
        DebugLog("[*] Scanning PCI Bus for USB EHCI Controller...");
        if (starlix::drivers::ehci::ehci_init()) {
            DebugLog("[OK] USB EHCI Host Controller initialized (built into kernel)");
        } else {
            DebugLog("[-] USB EHCI Host Controller Not Found; non-USB storage remains available.");
        }
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "ehci_init");

        /* Storage discovery is independent of USB controller availability.
           This scans PCI IDE/ATA PIO first, then any enumerated EHCI MSC LUNs. */
        starlix::storage::storage_init();
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "storage_init");

        DebugLog("[*] Scanning PCI Bus for USB UHCI Companion Controller...");
        if (starlix::drivers::uhci::uhci_init())
            DebugLog("[OK] USB UHCI companion controller initialized (built into kernel)");
        else
            DebugLog("[-] USB UHCI companion controller unavailable or safely disabled");
        CheckDmaAllocatorState(dma_allocator_initialized, dma_allocator_loss_reported,
                               "uhci_init");

        DebugLog("==============================================", 0x0038BDF8);
        DebugLog(" [OK] SYSTEM BOOT COMPLETE - ENTERING IDLE   ", 0x00FBBF24);
        DebugLog("==============================================", 0x0038BDF8);
#if !STARLIX_ENABLE_RING3_SMOKE_TEST
        /* Start the real stlcs.ect user component only after all executive,
           storage, paging, stack and scheduler prerequisites have completed.
           Successful IRETQ transfers control to CPL3 and does not return. */
        (void)starlix::loader::stlcs_production_launch();
#endif
#if STARLIX_ENABLE_RING3_SMOKE_TEST
        if (!starlix::process::user_mode_smoke_test::start()) {
            starlix::drivers::serial::print(
                "[RING3-TEST] NOT STARTED: reason=");
            starlix::drivers::serial::print(
                starlix::process::user_mode_smoke_test::failure_reason());
            starlix::drivers::serial::print(
                "; kernel remains in Ring 0.\r\n");
        }
#endif
    }

    for (;;) {
        if (g_BootInfo) {
            /* Deferred USB work, including HID periodic-report harvest, stays
               in normal context; the hard EHCI IRQ path never logs or renders. */
            (void)starlix::drivers::ehci::ehci_service_deferred_work();
            (void)starlix::drivers::ehci::ehci_hid_service();
            starlix::drivers::ehci::EHCIInputEvent input_event{};
            while (starlix::drivers::ehci::ehci_hid_event_poll(&input_event)) {
                starlix::gui::InputEvent gui_event{};
                gui_event.type = static_cast<starlix::gui::InputEventType>(
                    static_cast<uint8_t>(input_event.type));
                gui_event.code = input_event.code;
                gui_event.modifiers = input_event.modifiers;
                gui_event.buttons = input_event.buttons;
                gui_event.dx = input_event.dx;
                gui_event.dy = input_event.dy;
                gui_event.wheel = input_event.wheel;
                starlix::gui::gui_handle_input(&gui_event);
                serial_log_usb_input_event(input_event);
            }
        }
        if (g_BootInfo && starlix::drivers::ehci::ehci_service_port_changes()) {
            /* storage_init rebuilds the read-only block/partition/volume list
               after a root-port insertion or removal event. */
            starlix::storage::storage_init();
        }
        if (g_BootInfo && starlix::drivers::uhci::uhci_service_port_changes())
            starlix::storage::storage_init();
        if (g_BootInfo && starlix::drivers::ehci::ehci_service_hub_ports())
            starlix::storage::storage_init();

        if (!g_BootInfo) {
            asm volatile ("hlt");
            continue;
        }

        /* Avoid the lost-wakeup window between the pending check and HLT:
           an IRQ arriving after CLI stays pending until STI;HLT wakes us. */
        asm volatile ("cli" ::: "memory");
        if (starlix::drivers::ehci::ehci_port_change_pending() ||
            starlix::drivers::ehci::ehci_deferred_work_pending() ||
            starlix::drivers::uhci::uhci_port_change_pending()) {
            asm volatile ("sti" ::: "memory");
            continue;
        }
        asm volatile ("sti; hlt" ::: "memory");
    }
}

/* Phase 3 compatibility: include storage implementation in the existing main.cpp translation unit. */
#include "storage/storage.cpp"
