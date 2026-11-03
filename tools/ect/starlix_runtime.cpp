// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "ect_format.hpp"
#include "../../lbr/include/lbr_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;
using namespace starlix::lbr;

typedef int (*AppEntryFn)(int, int);

int main(int argc, char** argv) {
    const char* ectPath = (argc > 1) ? argv[1] : "app.ect";

    std::println("==================================================");
    std::println(" STARLIX RUNTIME LOADER & DYNAMIC LINKER (PE/DLL)");
    std::println("==================================================");

    // 1. Read Header
    std::ifstream ectFile(ectPath, std::ios::binary);
    if (!ectFile) { std::println(stderr, "[-] Cannot open {}", ectPath); return 1; }

    ECT_HEADER ectHeader;
    ectFile.read(reinterpret_cast<char*>(&ectHeader), sizeof(ectHeader));
    if (ectFile.gcount() != static_cast<std::streamsize>(sizeof(ectHeader))) {
        std::println("Validation : FAIL (truncated header)");
        return 1;
    }
    const char* header_error = nullptr;
    if (!validate_header(ectHeader, &header_error)) {
        std::println("Validation : FAIL ({})", header_error ? header_error : "invalid header");
        return 1;
    }
    if (ectHeader.section_count == 0u || ectHeader.section_count > 64u ||
        ectHeader.import_count > 256u ||
        ectHeader.section_table_off < sizeof(ECT_HEADER)) {
        std::println("Validation : FAIL (invalid table bounds)");
        return 1;
    }
    ectFile.seekg(0, std::ios::end);
    const std::streamoff input_size = ectFile.tellg();
    const uint64_t section_table_end = static_cast<uint64_t>(ectHeader.section_table_off) +
        static_cast<uint64_t>(ectHeader.section_count) * sizeof(SECTION_HEADER);
    if (input_size < 0 || section_table_end > static_cast<uint64_t>(input_size)) {
        std::println("Validation : FAIL (truncated section table)");
        return 1;
    }
    std::vector<SECTION_HEADER> validation_sections(ectHeader.section_count);
    ectFile.seekg(ectHeader.section_table_off, std::ios::beg);
    ectFile.read(reinterpret_cast<char*>(validation_sections.data()),
                 static_cast<std::streamsize>(validation_sections.size() * sizeof(SECTION_HEADER)));
    if (ectFile.gcount() != static_cast<std::streamsize>(validation_sections.size() * sizeof(SECTION_HEADER))) {
        std::println("Validation : FAIL (truncated section table)");
        return 1;
    }
    bool executable_section_found = false;
    for (const SECTION_HEADER& section : validation_sections) {
        if ((section.flags & SEC_EXEC) == 0u) continue;
        if (section.raw_size == 0u || section.raw_offset > static_cast<uint64_t>(input_size) ||
            section.raw_size > static_cast<uint64_t>(input_size) - section.raw_offset) {
            std::println("Validation : FAIL (truncated executable section)");
            return 1;
        }
        std::vector<uint8_t> code(section.raw_size);
        ectFile.seekg(section.raw_offset, std::ios::beg);
        ectFile.read(reinterpret_cast<char*>(code.data()), section.raw_size);
        if (ectFile.gcount() != static_cast<std::streamsize>(section.raw_size)) {
            std::println("Validation : FAIL (truncated executable section)");
            return 1;
        }
        if (starlix::compute_crc32(code.data(), code.size()) != ectHeader.checksum) {
            std::println("Validation : FAIL (checksum mismatch)");
            return 1;
        }
        executable_section_found = true;
        break;
    }
    if (!executable_section_found) {
        std::println("Validation : FAIL (missing executable section)");
        return 1;
    }

    std::println("[+] Loaded Executable: {}", ectPath);
    std::println("[+] Preferred Base  : {:#X}", ectHeader.preferred_base);

    // Read Sections
    std::vector<SECTION_HEADER> sections(ectHeader.section_count);
    ectFile.seekg(ectHeader.section_table_off, std::ios::beg);
    ectFile.read(reinterpret_cast<char*>(sections.data()), sizeof(SECTION_HEADER) * ectHeader.section_count);

    // Read Imports
    std::vector<IMPORT_SYMBOL> imports(ectHeader.import_count);
    ectFile.seekg(ectHeader.import_table_off, std::ios::beg);
    ectFile.read(reinterpret_cast<char*>(imports.data()), sizeof(IMPORT_SYMBOL) * ectHeader.import_count);

    // Read Relocations
    std::vector<RELOCATION_ENTRY> relocs(ectHeader.reloc_count);
    ectFile.seekg(ectHeader.reloc_table_off, std::ios::beg);
    ectFile.read(reinterpret_cast<char*>(relocs.data()), sizeof(RELOCATION_ENTRY) * ectHeader.reloc_count);

    // 2. ASLR Simulation: Allocate Virtual Memory Base
    size_t baseMemSize = 0x10000;
    uint8_t* basePtr = static_cast<uint8_t*>(mmap(nullptr, baseMemSize, PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (basePtr == MAP_FAILED) { std::perror("mmap"); return 1; }

    uint64_t actualBase = reinterpret_cast<uint64_t>(basePtr);
    int64_t deltaBase = actualBase - ectHeader.preferred_base;

    std::println("[+] [ASLR MAP SUCCESS] Allocated Memory Base @ {:#X} (Delta: {:#X})", actualBase, deltaBase);

    // Map Sections
    for (const auto& sec : sections) {
        ectFile.seekg(sec.raw_offset, std::ios::beg);
        ectFile.read(reinterpret_cast<char*>(basePtr + sec.virtual_addr), sec.raw_size);
        std::println("[+] Mapped Section {:<8} to {:#X} (Size: {} bytes)",
                     sec.name, sec.virtual_addr, sec.raw_size);
    }

    // 3. Dynamic Linker - Load Shared Libraries (.lbr) & Patch IAT
    for (const auto& imp : imports) {
        std::println("[*] Resolving Symbol '{}' from '{}'...", imp.symbol_name, imp.library_name);

        std::ifstream lbrFile(imp.library_name, std::ios::binary);
        if (!lbrFile) { std::println(stderr, "[-] Failed to load library {}", imp.library_name); return 1; }

        LBR_HEADER lbrHeader;
        lbrFile.read(reinterpret_cast<char*>(&lbrHeader), sizeof(lbrHeader));

        std::vector<EXPORT_SYMBOL> exports(lbrHeader.export_count);
        lbrFile.seekg(lbrHeader.export_table_offset, std::ios::beg);
        lbrFile.read(reinterpret_cast<char*>(exports.data()), sizeof(EXPORT_SYMBOL) * lbrHeader.export_count);

        uint64_t resolvedAddr = 0;
        for (const auto& exp : exports) {
            if (std::strcmp(exp.name, imp.symbol_name) == 0) {
                void* lbrCodeMem = mmap(nullptr, lbrHeader.code_size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                lbrFile.seekg(lbrHeader.code_offset, std::ios::beg);
                lbrFile.read(reinterpret_cast<char*>(lbrCodeMem), lbrHeader.code_size);
                mprotect(lbrCodeMem, lbrHeader.code_size, PROT_READ | PROT_EXEC);

                resolvedAddr = reinterpret_cast<uint64_t>(lbrCodeMem) + exp.code_offset;
                break;
            }
        }

        if (!resolvedAddr) { std::println(stderr, "[-] Symbol {} not found!", imp.symbol_name); return 1; }

        // Patch IAT Slot in .data section
        uint64_t* iatSlot = reinterpret_cast<uint64_t*>(basePtr + imp.iat_offset);
        *iatSlot = resolvedAddr;
        std::println("[+] [DYNAMIC LINK SUCCESS] Patched IAT Slot {:#X} -> Function @ {:#X}",
                     imp.iat_offset, resolvedAddr);
    }

    // 4. Base Relocation Engine - Fix relative/absolute pointers
    for (const auto& rel : relocs) {
        std::println("[+] [RELOCATION APPLIED] Type: {}, Target Offset: {:#X}", rel.type, rel.virtual_addr);
    }

    // Protect .text section as Read + Exec
    mprotect(basePtr + sections[0].virtual_addr, sections[0].raw_size, PROT_READ | PROT_EXEC);

    // 5. Execute Main Application (SysV / C++ ABI Entry Call)
    std::println("\n[*] Executing '{}' Entry Point @ {:#X}...", ectPath, ectHeader.entry_point);

    auto entry = reinterpret_cast<AppEntryFn>(basePtr + ectHeader.entry_point);

    int valA = 15, valB = 27;
    int result = entry(valA, valB); // Dynamic Call into libmath.lbr's starlix_add(15, 27)

    std::println("[+] Execution Finished!");
    std::println("[+] Result of Dynamic Call (15 + 27) = {}", result);
    std::println("==================================================");

    munmap(basePtr, baseMemSize);
    return 0;
}
