// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <string>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include "lbr_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::lbr;

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "libmath.lbr";
    std::string signing_key;
    if (argc > 2) {
        if (argc != 4 || std::strcmp(argv[2], "--sign") != 0 || argv[3][0] == '\0') {
            std::fprintf(stderr, "Usage: %s [output.lbr] [--sign key]\n", argv[0]);
            return 1;
        }
        signing_key = argv[3];
    }

    // Machine code for: int starlix_add(int a, int b) { return a + b; }
    // x86_64 SysV ABI: EDI = a, ESI = b -> LEA EAX, [RDI + RSI] ; RET
    std::vector<uint8_t> code = { 0x8D, 0x04, 0x37, 0xC3 };

    EXPORT_SYMBOL expSymbol{};
    std::strncpy(expSymbol.name, "starlix_add", sizeof(expSymbol.name) - 1);
    expSymbol.code_offset = 0;

    LBR_HEADER header;
    init_header(header);
    header.export_count = 1;
    header.export_table_offset = sizeof(LBR_HEADER);
    header.code_offset = sizeof(LBR_HEADER) + sizeof(EXPORT_SYMBOL);
    header.code_size = code.size();
    std::vector<uint8_t> integrity;
    integrity.insert(integrity.end(), reinterpret_cast<const uint8_t*>(&expSymbol),
                     reinterpret_cast<const uint8_t*>(&expSymbol) + sizeof(expSymbol));
    integrity.insert(integrity.end(), code.begin(), code.end());
    header.checksum = starlix::compute_crc32(integrity.data(), integrity.size());

    std::vector<uint8_t> signature;
    if (!signing_key.empty()) {
        signature.resize(EVP_MAX_MD_SIZE);
        unsigned int signature_size = 0u;
        if (!HMAC(EVP_sha256(), signing_key.data(), static_cast<int>(signing_key.size()),
                  integrity.data(), integrity.size(), signature.data(), &signature_size)) {
            std::fprintf(stderr, "lbrpack: HMAC-SHA256 signing failed\n");
            return 1;
        }
        signature.resize(signature_size);
        header.has_signature = 1u;
        header.signature_offset = header.code_offset + code.size();
        header.signature_size = static_cast<uint32_t>(signature.size());
    }

    std::ofstream out(outPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&expSymbol), sizeof(expSymbol));
    out.write(reinterpret_cast<const char*>(code.data()), code.size());
    if (!signature.empty())
        out.write(reinterpret_cast<const char*>(signature.data()), signature.size());
    out.close();
    if (!out) {
        std::fprintf(stderr, "lbrpack: failed writing %s\n", outPath);
        return 1;
    }

    std::printf("[lbrpack] Created Shared Library '%s' (Export: starlix_add%s)\n", outPath,
                signature.empty() ? "" : ", HMAC-SHA256 signed");
    return 0;
}
