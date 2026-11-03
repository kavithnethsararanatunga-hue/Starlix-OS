// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kavith Ranatunga

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem>

#include "ssp/sspfs.hpp"

namespace fs = std::filesystem;

struct FileItem {
    std::string relPath;
    fs::path fullPath;
    uint64_t size;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <input_dir> <output_file> [--size <MB>]\n", argv[0]);
        return 1;
    }

    std::string inputDir  = argv[1];
    std::string outputFile = argv[2];
    uint64_t paddedSize = 0;

    for (int i = 3; i < argc; i++) {
        if (std::string(argv[i]) == "--size" && i + 1 < argc) {
            paddedSize = (uint64_t)std::stoull(argv[i + 1]) * 1024ULL * 1024ULL;
            i++;
        }
    }

    std::vector<FileItem> files;

    for (auto& p : fs::recursive_directory_iterator(inputDir)) {
        if (!p.is_regular_file()) continue;
        std::string rel = fs::relative(p.path(), inputDir).string();
        for (auto& c : rel) if (c == '\\') c = '/';
        rel = "/" + rel;
        files.push_back({rel, p.path(), (uint64_t)fs::file_size(p.path())});
    }

    std::printf("[mkssp] Found %zu files in %s\n", files.size(), inputDir.c_str());

    uint32_t fileCount = (uint32_t)files.size();
    uint64_t sbSize = sizeof(SSP_SUPERBLOCK);
    uint64_t ftSize = (uint64_t)fileCount * sizeof(SSP_FILE_ENTRY);
    uint64_t dataStart = sbSize + ftSize;

    const uint64_t ALIGN = 512;
    if (dataStart % ALIGN != 0) dataStart += ALIGN - (dataStart % ALIGN);

    std::vector<SSP_FILE_ENTRY> entries(fileCount);
    uint64_t curOffset = dataStart;

    for (uint32_t i = 0; i < fileCount; i++) {
        std::memset(&entries[i], 0, sizeof(SSP_FILE_ENTRY));
        std::strncpy((char*)entries[i].name, files[i].relPath.c_str(), sizeof(entries[i].name) - 1);
        entries[i].offset = curOffset;
        entries[i].size = files[i].size;
        entries[i].attributes = 0;
        curOffset += files[i].size;
        if (curOffset % 16 != 0) curOffset += 16 - (curOffset % 16);
    }

    uint64_t totalSize = curOffset;
    if (paddedSize > totalSize) totalSize = paddedSize;

    SSP_SUPERBLOCK sb;
    std::memset(&sb, 0, sizeof(sb));
    std::memcpy(sb.magic, SSP_MAGIC, 8);
    sb.version = SSP_VERSION;
    sb.flags = SSP_FLAG_READONLY;
    sb.total_size = totalSize;
    sb.file_count = fileCount;
    sb.file_table_offset = (uint32_t)sbSize;
    sb.file_table_entry_size = (uint32_t)sizeof(SSP_FILE_ENTRY);
    sb.data_start_offset = (uint32_t)dataStart;
    std::strncpy((char*)sb.volume_name, "STARLIX_SSP", sizeof(sb.volume_name) - 1);
    sb.checksum = 0;

    std::ofstream out(outputFile, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "[mkssp] Cannot open output file: %s\n", outputFile.c_str());
        return 1;
    }

    // Pre-fill entire image with zero bytes
    std::vector<char> zeroBuf(1024 * 1024, 0);
    uint64_t written = 0;
    while (written < totalSize) {
        uint64_t chunk = std::min<uint64_t>(zeroBuf.size(), totalSize - written);
        out.write(zeroBuf.data(), (std::streamsize)chunk);
        written += chunk;
    }

    out.seekp(0);
    out.write((const char*)&sb, sizeof(sb));

    out.seekp(sb.file_table_offset);
    out.write((const char*)entries.data(), (std::streamsize)ftSize);

    for (uint32_t i = 0; i < fileCount; i++) {
        std::ifstream in(files[i].fullPath, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "[mkssp] Cannot read: %s\n", files[i].fullPath.string().c_str());
            continue;
        }
        std::vector<char> buf(files[i].size);
        in.read(buf.data(), (std::streamsize)files[i].size);
        out.seekp((std::streamoff)entries[i].offset);
        out.write(buf.data(), (std::streamsize)files[i].size);
        std::printf("[mkssp]  + %-40s %10llu bytes @ offset %llu\n",
                    entries[i].name,
                    (unsigned long long)entries[i].size,
                    (unsigned long long)entries[i].offset);
    }

    out.close();
    std::printf("[mkssp] SSP image created: %s (%llu bytes total)\n",
                outputFile.c_str(), (unsigned long long)totalSize);
    return 0;
}
