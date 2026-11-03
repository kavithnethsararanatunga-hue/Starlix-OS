#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include "driver/sys_validate.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: sys_validate_probe <package.sys>\n";
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "REJECT: cannot open package\n";
        return 1;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());

    const auto result = starlix::sys::ValidateSysPackage(
        bytes.data(), static_cast<std::uint64_t>(bytes.size()));

    if (result != starlix::sys::SYS_VALIDATE_RESULT::Valid) {
        std::cerr << "REJECT: SYS validator result "
                  << static_cast<std::uint32_t>(result) << "\n";
        return 1;
    }

    std::cout << "PASS: C++ SYS/ELF validator accepted package; "
                 "payload was not executed.\n";
    return 0;
}
