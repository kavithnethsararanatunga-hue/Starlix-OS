// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <print>
#include "ect_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;

int main(int argc, char** argv) {
    const char* binPath = (argc > 1) ? argv[1] : "kernel/build/kernel.bin";
    const char* outPath = (argc > 2) ? argv[2] : "RxKernel1.0.ect";
    uint64_t entry = (argc > 3) ? std::strtoull(argv[3], nullptr, 0) : 0x200000;

    std::ifstream in(binPath, std::ios::binary);
    if (!in) { std::println(stderr, "cannot open {}", binPath); return 1; }
    std::vector<uint8_t> code((std::istreambuf_iterator<char>(in)), {});

    SECTION_HEADER sec{};
    std::memset(&sec, 0, sizeof(sec));
    std::strncpy(sec.name, ".text", 8);
    sec.virtual_addr = 0x200000;
    sec.raw_size = (uint32_t)code.size();
    sec.flags = SEC_READ | SEC_EXEC;

    ECT_HEADER h;
    init_header(h);
    h.section_count = 1;
    h.section_table_off = sizeof(ECT_HEADER);
    h.import_count = 0;
    h.import_table_off = sizeof(ECT_HEADER) + sizeof(sec);
    h.reloc_count = 0;
    h.reloc_table_off = h.import_table_off;
    h.entry_point = entry;
    h.preferred_base = 0x200000;
    sec.raw_offset = (uint32_t)(sizeof(ECT_HEADER) + sizeof(sec));
    h.checksum = starlix::compute_crc32(code.data(), code.size());

    std::ofstream out(outPath, std::ios::binary);
    out.write((char*)&h, sizeof(h));
    out.write((char*)&sec, sizeof(sec));
    out.write((char*)code.data(), code.size());
    out.close();

    std::println("[pack_kernel_ect] {} ({} code bytes, entry {:#x})", outPath, code.size(), entry);
    return 0;
}
