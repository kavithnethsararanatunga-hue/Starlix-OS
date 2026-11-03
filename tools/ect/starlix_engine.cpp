// Starlix OS - OpenRPX xsOft - Trade secret. See project LICENSE.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "ect_format.hpp"
#include "../common/crc32.hpp"

using namespace starlix::ect;
typedef int (*EntryFn)();

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file.ect>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "starlix-engine: cannot open %s\n", path);
        return 1;
    }

    ECT_HEADER header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (in.gcount() != (std::streamsize)sizeof(header)) {
        std::printf("Validation : FAIL (file too small)\n");
        return 1;
    }

    const char* reason = nullptr;
    if (!validate_header(header, &reason)) {
        std::printf("Validation : FAIL (%s)\n", reason ? reason : "unknown");
        return 1;
    }

    std::vector<uint8_t> code(header.code_size);
    in.seekg(header.entry_offset, std::ios::beg);
    in.read(reinterpret_cast<char*>(code.data()), (std::streamsize)header.code_size);
    if ((uint64_t)in.gcount() != header.code_size) {
        std::printf("Validation : FAIL (truncated code segment)\n");
        return 1;
    }

    if (starlix::compute_crc32(code.data(), code.size()) != header.checksum) {
        std::printf("Validation : FAIL (checksum mismatch)\n");
        return 1;
    }

    size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
    size_t mapSize = ((code.size() + pageSize - 1) / pageSize) * pageSize;
    if (mapSize == 0) mapSize = pageSize;

    void* mem = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "starlix-engine: mmap failed\n");
        return 1;
    }
    std::memcpy(mem, code.data(), code.size());

    if (mprotect(mem, mapSize, PROT_READ | PROT_EXEC) != 0) {
        std::fprintf(stderr, "starlix-engine: mprotect failed\n");
        munmap(mem, mapSize);
        return 1;
    }

    std::printf("Execution : ENTERED\n");
    EntryFn entry = reinterpret_cast<EntryFn>(mem);
    int exitStatus = entry();              // <-- real machine code runs here
    std::printf("Exit Status : %d\n", exitStatus);
    std::printf("Execution : %s\n", (exitStatus == 0) ? "SUCCESS" : "FAILURE");

    munmap(mem, mapSize);
    return exitStatus == 0 ? 0 : 1;
}
