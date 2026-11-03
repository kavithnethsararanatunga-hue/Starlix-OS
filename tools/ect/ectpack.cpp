// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <print>
#include <cstring>
#include <vector>
#include <fstream>
#include "ect_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "app.ect";

    // 1. Setup Sections
    // .text section (Code):
    // MOV RAX, [RIP + 0x0FF9]  -> 48 8B 05 F9 0F 00 00 (0x1007 + 0x0FF9 = 0x2000 .data IAT Slot)
    // CALL RAX                 -> FF D0
    // RET                      -> C3
    std::vector<uint8_t> textSec = {
        0x48, 0x8B, 0x05, 0xF9, 0x0F, 0x00, 0x00,
        0xFF, 0xD0,
        0xC3
    };

    // .data section (8 bytes IAT Slot):
    std::vector<uint8_t> dataSec(8, 0x00);

    SECTION_HEADER secs[2];
    std::memset(secs, 0, sizeof(secs));

    // Section 1: .text
    std::strncpy(secs[0].name, ".text", 8);
    secs[0].virtual_addr = 0x1000;
    secs[0].raw_size = textSec.size();
    secs[0].flags = SEC_READ | SEC_EXEC;

    // Section 2: .data
    std::strncpy(secs[1].name, ".data", 8);
    secs[1].virtual_addr = 0x2000;
    secs[1].raw_size = dataSec.size();
    secs[1].flags = SEC_READ | SEC_WRITE;

    // 2. Setup Import Symbol
    IMPORT_SYMBOL impSymbol{};
    std::strncpy(impSymbol.library_name, "libmath.lbr", 32);
    std::strncpy(impSymbol.symbol_name, "starlix_add", 32);
    impSymbol.iat_offset = 0x2000; // Points to .data section

    // 3. Setup ECT Header
    ECT_HEADER header;
    init_header(header);
    header.section_count = 2;
    header.section_table_off = sizeof(ECT_HEADER);
    header.import_count = 1;
    header.import_table_off = sizeof(ECT_HEADER) + sizeof(secs);
    header.entry_point = 0x1000; // Entry point

    uint32_t fileOffset = sizeof(ECT_HEADER) + sizeof(secs) + sizeof(impSymbol);
    secs[0].raw_offset = fileOffset;
    fileOffset += secs[0].raw_size;
    secs[1].raw_offset = fileOffset;

    header.checksum = starlix::compute_crc32(textSec.data(), textSec.size());

    std::ofstream out(outPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(secs), sizeof(secs));
    out.write(reinterpret_cast<const char*>(&impSymbol), sizeof(impSymbol));
    out.write(reinterpret_cast<const char*>(textSec.data()), textSec.size());
    out.write(reinterpret_cast<const char*>(dataSec.data()), dataSec.size());
    out.close();

    std::println("[ectpack] Created Multi-Section Executable '{}' (C++26)", outPath);
    std::println("[ectpack]   Sections: .text (RX @ 0x1000), .data (RW @ 0x2000)");
    std::println("[ectpack]   Import  : libmath.lbr -> starlix_add");
    return 0;
}
