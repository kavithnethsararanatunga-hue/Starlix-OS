#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <filesystem>
#include <string>
#include <elf.h>
#include "driver/sys_format.hpp"

static std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool copy_bounded(char* dst, std::size_t cap, const char* src) {
    if (!src || !*src || std::strlen(src) >= cap) return false;
    std::memcpy(dst, src, std::strlen(src));
    return true;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: pack_sys input.o output.sys module_name entry_symbol\n";
        return 2;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open input object: " << argv[1] << "\n";
        return 1;
    }

    std::vector<std::uint8_t> payload(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    if (payload.size() < sizeof(Elf64_Ehdr)) {
        std::cerr << "ELF object is too small\n";
        return 1;
    }

    Elf64_Ehdr eh{};
    std::memcpy(&eh, payload.data(), sizeof(eh));

    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_type != ET_REL || eh.e_machine != EM_X86_64) {
        std::cerr << "Expected a little-endian x86-64 ELF relocatable object\n";
        return 1;
    }

    starlix::sys::SYS_HEADER header{};
    std::memcpy(header.magic, "STSYS001", 8);
    header.header_size = sizeof(header);
    header.version_major = starlix::sys::VERSION_MAJOR;
    header.version_minor = starlix::sys::VERSION_MINOR;
    header.architecture = starlix::sys::ARCH_X86_64;
    header.payload_format = starlix::sys::PAYLOAD_ELF64_RELOCATABLE;
    header.payload_size = payload.size();
    header.payload_crc32 = crc32_bytes(payload.data(), payload.size());
    header.flags = starlix::sys::FLAG_REQUIRES_RELOCATION;

    if (!copy_bounded(header.module_name, sizeof(header.module_name), argv[3]) ||
        !copy_bounded(header.entry_symbol, sizeof(header.entry_symbol), argv[4])) {
        std::cerr << "Module name or entry symbol is too long/empty\n";
        return 2;
    }

    const std::filesystem::path output(argv[2]);
    const std::filesystem::path temporary = output.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "Cannot create output file: " << temporary << "\n";
            return 1;
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        out.close();
        if (!out) {
            std::cerr << "Failed while writing output package\n";
            return 1;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, output, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        std::cerr << "Cannot finalize output package: " << ec.message() << "\n";
        return 1;
    }

    std::cout << "[pack_sys] " << output
              << " | payload=" << payload.size()
              << " bytes | total=" << sizeof(header) + payload.size()
              << " bytes | module=" << header.module_name
              << " | entry=" << header.entry_symbol << "\n";
    return 0;
}
