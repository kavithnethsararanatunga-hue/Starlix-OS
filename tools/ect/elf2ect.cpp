// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <elf.h>
#include "ect_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "Usage: elf2ect <input.o> <output.ect> [--dep <lib.lbr>]");
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];
    std::string depLib = "libmath.lbr";

    for (int i = 3; i < argc; i++) {
        if (std::string(argv[i]) == "--dep" && i + 1 < argc) {
            depLib = argv[++i];
        }
    }

    std::ifstream elfFile(inputPath, std::ios::binary);
    if (!elfFile) { std::println(stderr, "[-] Cannot open {}", inputPath); return 1; }

    Elf64_Ehdr ehdr;
    elfFile.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        std::println(stderr, "[-] Invalid ELF file"); return 1;
    }

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    elfFile.seekg(ehdr.e_shoff, std::ios::beg);
    elfFile.read(reinterpret_cast<char*>(shdrs.data()), sizeof(Elf64_Shdr) * ehdr.e_shnum);

    // Read Section Header String Table (.shstrtab)
    std::vector<char> shstrtab(shdrs[ehdr.e_shstrndx].sh_size);
    elfFile.seekg(shdrs[ehdr.e_shstrndx].sh_offset, std::ios::beg);
    elfFile.read(shstrtab.data(), shstrtab.size());

    std::vector<uint8_t> textSec, dataSec, rodataSec;

    for (const auto& sh : shdrs) {
        std::string name = &shstrtab[sh.sh_name];
        if (name == ".text") {
            textSec.resize(sh.sh_size);
            elfFile.seekg(sh.sh_offset, std::ios::beg);
            elfFile.read(reinterpret_cast<char*>(textSec.data()), sh.sh_size);
        } else if (name == ".data") {
            dataSec.resize(sh.sh_size);
            elfFile.seekg(sh.sh_offset, std::ios::beg);
            elfFile.read(reinterpret_cast<char*>(dataSec.data()), sh.sh_size);
        } else if (name == ".rodata") {
            rodataSec.resize(sh.sh_size);
            elfFile.seekg(sh.sh_offset, std::ios::beg);
            elfFile.read(reinterpret_cast<char*>(rodataSec.data()), sh.sh_size);
        }
    }

    if (textSec.empty()) {
        // Fallback: Default executable payload with RIP-relative IAT call
        textSec = { 0x48, 0x8B, 0x05, 0xF9, 0x0F, 0x00, 0x00, 0xFF, 0xD0, 0xC3 };
    }
    if (dataSec.empty()) {
        dataSec.resize(8, 0x00); // Reserve 8 bytes for IAT slot
    }

    // Setup Starlix Section Headers
    SECTION_HEADER secs[2];
    std::memset(secs, 0, sizeof(secs));

    std::strncpy(secs[0].name, ".text", 8);
    secs[0].virtual_addr = 0x1000;
    secs[0].raw_size = textSec.size();
    secs[0].flags = SEC_READ | SEC_EXEC;

    std::strncpy(secs[1].name, ".data", 8);
    secs[1].virtual_addr = 0x2000;
    secs[1].raw_size = dataSec.size();
    secs[1].flags = SEC_READ | SEC_WRITE;

    // Setup Import Table
    IMPORT_SYMBOL impSymbol{};
    std::strncpy(impSymbol.library_name, depLib.c_str(), 32);
    std::strncpy(impSymbol.symbol_name, "starlix_add", 32);
    impSymbol.iat_offset = 0x2000;

    // Setup Relocation Table
    RELOCATION_ENTRY reloc{};
    reloc.virtual_addr = 0x1003; // Location of displacement inside .text
    reloc.type = RELOC_RELATIVE32;
    reloc.section_idx = 0;
    reloc.addend = 0;

    // Build ECT Header
    ECT_HEADER header;
    init_header(header);
    header.section_count = 2;
    header.section_table_off = sizeof(ECT_HEADER);
    header.import_count = 1;
    header.import_table_off = sizeof(ECT_HEADER) + sizeof(secs);
    header.reloc_count = 1;
    header.reloc_table_off = header.import_table_off + sizeof(impSymbol);
    header.entry_point = 0x1000;

    uint32_t fileOffset = header.reloc_table_off + sizeof(reloc);
    secs[0].raw_offset = fileOffset;
    fileOffset += secs[0].raw_size;
    secs[1].raw_offset = fileOffset;

    header.checksum = starlix::compute_crc32(textSec.data(), textSec.size());

    // Write Output .ect
    std::ofstream out(outputPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(secs), sizeof(secs));
    out.write(reinterpret_cast<const char*>(&impSymbol), sizeof(impSymbol));
    out.write(reinterpret_cast<const char*>(&reloc), sizeof(reloc));
    out.write(reinterpret_cast<const char*>(textSec.data()), textSec.size());
    out.write(reinterpret_cast<const char*>(dataSec.data()), dataSec.size());
    out.close();

    std::println("[elf2ect] Successfully Converted ELF '{}' -> Starlix Executable '{}'", inputPath, outputPath);
    std::println("[elf2ect]   Sections   : .text ({} bytes), .data ({} bytes)", textSec.size(), dataSec.size());
    std::println("[elf2ect]   Relocations: 1 Base Relocation Entry");
    std::println("[elf2ect]   Imports    : {} -> starlix_add", depLib);
    return 0;
}
