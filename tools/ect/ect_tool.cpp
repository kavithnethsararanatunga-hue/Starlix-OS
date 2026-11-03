// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include "ect_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;

static void field(const char* label, const std::string& value) {
    std::printf("%-17s: %s\n", label, value.c_str());
}

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "inspect") {
        std::fprintf(stderr, "Usage: %s inspect <file.ect>\n", argv[0]);
        return 1;
    }

    const char* path = argv[2];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "ect-tool: cannot read file: %s\n", path);
        return 1;
    }

    ECT_HEADER header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));

    const char* reason = nullptr;
    bool valid = validate_header(header, &reason);
    if (valid && (header.section_count == 0u || header.section_count > 64u ||
                  header.section_table_off < sizeof(ECT_HEADER))) {
        valid = false;
        reason = "invalid section table";
    }
    std::vector<SECTION_HEADER> sections;
    if (valid) {
        in.seekg(0, std::ios::end);
        const std::streamoff file_size = in.tellg();
        const uint64_t table_end = static_cast<uint64_t>(header.section_table_off) +
            static_cast<uint64_t>(header.section_count) * sizeof(SECTION_HEADER);
        if (file_size < 0 || table_end > static_cast<uint64_t>(file_size)) {
            valid = false;
            reason = "truncated section table";
        } else {
            sections.resize(header.section_count);
            in.seekg(header.section_table_off, std::ios::beg);
            in.read(reinterpret_cast<char*>(sections.data()),
                    static_cast<std::streamsize>(sections.size() * sizeof(SECTION_HEADER)));
            if (in.gcount() != static_cast<std::streamsize>(sections.size() * sizeof(SECTION_HEADER))) {
                valid = false;
                reason = "truncated section table";
            }
        }
    }
    if (valid) {
        bool found_code = false;
        std::vector<uint8_t> code;
        in.seekg(0, std::ios::end);
        const uint64_t file_size = static_cast<uint64_t>(in.tellg());
        for (const SECTION_HEADER& section : sections) {
            if ((section.flags & SEC_EXEC) == 0u) continue;
            if (section.raw_size == 0u || section.raw_offset > file_size ||
                section.raw_size > file_size - section.raw_offset) {
                valid = false;
                reason = "truncated executable section";
                break;
            }
            code.resize(section.raw_size);
            in.seekg(section.raw_offset, std::ios::beg);
            in.read(reinterpret_cast<char*>(code.data()), section.raw_size);
            if (in.gcount() != static_cast<std::streamsize>(section.raw_size)) {
                valid = false;
                reason = "truncated executable section";
                break;
            }
            found_code = true;
            break;
        }
        if (valid && !found_code) {
            valid = false;
            reason = "missing executable section";
        } else if (valid && starlix::compute_crc32(code.data(), code.size()) != header.checksum) {
            valid = false;
            reason = "checksum mismatch";
        }
    }

    std::printf("STARLIX ECT BINARY INSPECTOR\n\n");
    field("File", path);
    field("Magic", std::string(header.magic, 2));
    field("Binary Banner", header.banner);
    field("Format", header.format_name);
    field("Version", std::to_string(header.version_major) + "." + std::to_string(header.version_minor));
    field("Image Type", header.image_type == 1 ? "Executable" : "Unknown");
    field("Architecture", header.architecture == 1 ? "x86_64" : "Unknown");
    field("Header Size", std::to_string(header.header_size) + " bytes");
    field("Sections", std::to_string(header.section_count));
    field("Imports", std::to_string(header.import_count));
    field("Validation", valid ? "PASS" : (std::string("FAIL (") + (reason ? reason : "unknown") + ")"));

    return valid ? 0 : 1;
}
