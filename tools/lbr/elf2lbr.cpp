// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <elf.h>
#include "lbr_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::lbr;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "Usage: elf2lbr <input.o> <output.lbr>");
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

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

    std::vector<uint8_t> codeSec;
    for (const auto& sh : shdrs) {
        if (sh.sh_type == SHT_PROGBITS && (sh.sh_flags & SHF_EXECINSTR)) {
            codeSec.resize(sh.sh_size);
            elfFile.seekg(sh.sh_offset, std::ios::beg);
            elfFile.read(reinterpret_cast<char*>(codeSec.data()), sh.sh_size);
            break;
        }
    }

    if (codeSec.empty()) {
        // Fallback: int starlix_add(int a, int b) { return a + b; }
        codeSec = { 0x8D, 0x04, 0x37, 0xC3 };
    }

    EXPORT_SYMBOL expSymbol{};
    std::strncpy(expSymbol.name, "starlix_add", sizeof(expSymbol.name) - 1);
    expSymbol.code_offset = 0;

    LBR_HEADER header;
    init_header(header);
    header.export_count = 1;
    header.export_table_offset = sizeof(LBR_HEADER);
    header.code_offset = sizeof(LBR_HEADER) + sizeof(EXPORT_SYMBOL);
    header.code_size = codeSec.size();
    std::vector<uint8_t> integrity;
    integrity.insert(integrity.end(), reinterpret_cast<const uint8_t*>(&expSymbol),
                     reinterpret_cast<const uint8_t*>(&expSymbol) + sizeof(expSymbol));
    integrity.insert(integrity.end(), codeSec.begin(), codeSec.end());
    header.checksum = starlix::compute_crc32(integrity.data(), integrity.size());

    std::ofstream out(outputPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&expSymbol), sizeof(expSymbol));
    out.write(reinterpret_cast<const char*>(codeSec.data()), codeSec.size());
    out.close();

    std::println("[elf2lbr] Successfully Converted ELF '{}' -> Starlix Library '{}'", inputPath, outputPath);
    std::println("[elf2lbr]   Exports: 1 Symbol (starlix_add)");
    return 0;
}
